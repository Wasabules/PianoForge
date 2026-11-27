# SPDX-License-Identifier: AGPL-3.0-or-later

# OBS plugin: the PianoForge sources (audio, keyboard, text) and chapter markers, fed by the application through
# shared memory (engine/include/pianoforge/audio_feed.hpp). No virtual cable, no system mixer in between.
#
# Windows: built against the installed OBS Studio. The libobs and frontend API headers of the same version are fetched
# (and checked against their hash); the import libraries are made from the installed obs.dll's and
# obs-frontend-api.dll's exports. The target install-obs-plugin copies the plugin where OBS looks for plugins of every
# user, no administrator rights needed, OBS running or not (the new plugin then loads at its next start):
# %ProgramData%/obs-studio/plugins/pianoforge-obs.
# Linux: built against the system's libobs and frontend API (package libobs-dev or obs-studio's development files);
# install-obs-plugin copies it to ~/.config/obs-studio/plugins/pianoforge-obs. macOS: against OBS's libobs and frontend
# API (their CMake packages), as a pianoforge-obs.plugin bundle.
#
# It builds alone too (inside a Flatpak OBS's SDK, or for a package): cmake -S obs -B build/obs.

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    cmake_minimum_required(VERSION 3.25)
    project(PianoForgeObs LANGUAGES C CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
    include(CTest)
    include(FetchContent)
endif()
get_filename_component(pianoforge_root "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE) # the engine's sources and headers

set(PIANOFORGE_OBS_VERSION "33.0.1" CACHE STRING "OBS Studio version the plugin is built against (its libobs headers)")
set(PIANOFORGE_OBS_HASH "9174ee0e3677c92d0d060bdce4c5d44dfee9fd3afddfb6333f098fd029b8de2b" CACHE STRING
    "SHA-256 of the OBS Studio source archive of that version")

add_library(pianoforge-obs MODULE
    src/plugin.cpp
    src/audio_source.cpp
    src/keyboard_source.cpp
    src/text_source.cpp
    src/chapters.cpp
    src/look_sync.cpp
    ${pianoforge_root}/engine/src/audio_feed.cpp  # built here, without the engine's AVX2 flags
    ${pianoforge_root}/engine/src/look_channel.cpp)
target_include_directories(pianoforge-obs PRIVATE ${pianoforge_root}/engine/include)
target_compile_features(pianoforge-obs PRIVATE cxx_std_20)
set_target_properties(pianoforge-obs PROPERTIES PREFIX "" OUTPUT_NAME "pianoforge-obs")

# The overlays' logic without OBS (keyboard geometry and state, text template).
if(BUILD_TESTING)
    add_executable(pianoforge-obs-overlay-test tests/overlay_test.cpp)
    target_include_directories(pianoforge-obs-overlay-test PRIVATE ${pianoforge_root}/engine/include)
    target_compile_features(pianoforge-obs-overlay-test PRIVATE cxx_std_20)
    if(MSVC)
        target_compile_options(pianoforge-obs-overlay-test PRIVATE /utf-8 /W4)
        target_compile_definitions(pianoforge-obs-overlay-test PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
    add_test(NAME obs_overlay COMMAND pianoforge-obs-overlay-test)
endif()

if(WIN32)
    set(PIANOFORGE_OBS_DIR "C:/Program Files/obs-studio" CACHE PATH "OBS Studio installation")

    FetchContent_Declare(obs_sources
        URL      https://github.com/obsproject/obs-studio/archive/refs/tags/${PIANOFORGE_OBS_VERSION}.tar.gz
        URL_HASH SHA256=${PIANOFORGE_OBS_HASH}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR do-not-build) # the headers only
    FetchContent_MakeAvailable(obs_sources)

    # An import library from an installed DLL: its exports (dumpbin, next to the linker) become a .def file for lib.exe.
    get_filename_component(msvc_tools "${CMAKE_LINKER}" DIRECTORY)
    find_program(PIANOFORGE_DUMPBIN dumpbin HINTS "${msvc_tools}" REQUIRED)
    function(pianoforge_import_library name dll out)
        set(lib "${CMAKE_CURRENT_BINARY_DIR}/${name}.lib")
        if(NOT EXISTS "${lib}" OR "${dll}" IS_NEWER_THAN "${lib}")
            execute_process(COMMAND "${PIANOFORGE_DUMPBIN}" /nologo /exports "${dll}" OUTPUT_VARIABLE exports
                            RESULT_VARIABLE dumped)
            if(NOT dumped EQUAL 0)
                message(FATAL_ERROR "dumpbin could not read ${dll}")
            endif()
            string(REGEX MATCHALL "\n +[0-9]+ +[0-9A-Fa-f]+ +[0-9A-Fa-f]+ +[A-Za-z_][A-Za-z0-9_]*" rows "${exports}")
            set(def "LIBRARY ${name}\nEXPORTS\n")
            foreach(row IN LISTS rows)
                string(REGEX REPLACE ".* ([A-Za-z_][A-Za-z0-9_]*)$" "\\1" symbol "${row}")
                string(APPEND def "    ${symbol}\n")
            endforeach()
            file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/${name}.def" "${def}")
            execute_process(COMMAND "${CMAKE_AR}" /nologo /def:${CMAKE_CURRENT_BINARY_DIR}/${name}.def /machine:x64
                                    /out:${lib} RESULT_VARIABLE made)
            if(NOT made EQUAL 0)
                message(FATAL_ERROR "lib.exe could not make ${lib}")
            endif()
        endif()
        set(${out} "${lib}" PARENT_SCOPE)
    endfunction()
    pianoforge_import_library(obs "${PIANOFORGE_OBS_DIR}/bin/64bit/obs.dll" obs_lib)
    pianoforge_import_library(obs-frontend-api "${PIANOFORGE_OBS_DIR}/bin/64bit/obs-frontend-api.dll" frontend_lib)

    # The header libobs's build generates (install paths, release flags): the values of OBS's Windows build, unused
    # by the plugin.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/obsconfig.h"
         "#pragma once\n#define OBS_DATA_PATH \"../../data\"\n#define OBS_PLUGIN_PATH \"../../obs-plugins/64bit\"\n"
         "#define OBS_PLUGIN_DESTINATION \"obs-plugins/64bit\"\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n")
    set(obs_includes "${obs_sources_SOURCE_DIR}/libobs" "${obs_sources_SOURCE_DIR}/frontend/api"
                     "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_include_directories(pianoforge-obs SYSTEM PRIVATE ${obs_includes})
    target_link_libraries(pianoforge-obs PRIVATE "${obs_lib}" "${frontend_lib}")
    target_compile_options(pianoforge-obs PRIVATE /utf-8 /W3)

    file(TO_CMAKE_PATH "$ENV{ProgramData}" program_data)
    set(plugin_root "${program_data}/obs-studio/plugins/pianoforge-obs")
    add_custom_target(install-obs-plugin
        COMMAND ${CMAKE_COMMAND} -DDLL=$<TARGET_FILE:pianoforge-obs>
                -DLOCALES=${CMAKE_CURRENT_SOURCE_DIR}/data/locale -DDEST=${plugin_root}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_plugin.cmake
        DEPENDS pianoforge-obs
        COMMENT "Installing the PianoForge OBS plugin into ${plugin_root}")
else()
    find_package(libobs REQUIRED)
    find_package(obs-frontend-api REQUIRED)
    target_link_libraries(pianoforge-obs PRIVATE OBS::libobs OBS::obs-frontend-api)
    if(APPLE)
        # macOS: a bundle, pianoforge-obs.plugin, its data in Contents/Resources (where OBS looks for it there), in
        # ~/Library/Application Support/obs-studio/plugins.
        set_target_properties(pianoforge-obs PROPERTIES BUNDLE TRUE BUNDLE_EXTENSION plugin)
        add_custom_command(TARGET pianoforge-obs POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_CURRENT_SOURCE_DIR}/data
                    "$<TARGET_BUNDLE_CONTENT_DIR:pianoforge-obs>/Resources")
        set(obs_plugins "$ENV{HOME}/Library/Application Support/obs-studio/plugins")
        add_custom_target(install-obs-plugin
            COMMAND ${CMAKE_COMMAND} -E copy_directory "$<TARGET_BUNDLE_DIR:pianoforge-obs>"
                    "${obs_plugins}/pianoforge-obs.plugin"
            DEPENDS pianoforge-obs
            COMMENT "Installing the PianoForge OBS plugin into ${obs_plugins}")
    else()
        # Linux: OBS's per-user plugin folder, in its configuration folder ($XDG_CONFIG_HOME, ~/.config by default).
        # Built inside a Flatpak OBS's SDK (flatpak run --devel ... com.obsproject.Studio), that variable is the
        # Flatpak's own, ~/.var/app/com.obsproject.Studio/config: the plugin lands where that OBS looks.
        if(DEFINED ENV{XDG_CONFIG_HOME} AND NOT "$ENV{XDG_CONFIG_HOME}" STREQUAL "")
            set(obs_config_default "$ENV{XDG_CONFIG_HOME}")
        else()
            set(obs_config_default "$ENV{HOME}/.config")
        endif()
        set(PIANOFORGE_OBS_PLUGINS_DIR "${obs_config_default}/obs-studio/plugins" CACHE PATH "OBS's per-user plugin folder")
        set(plugin_root "${PIANOFORGE_OBS_PLUGINS_DIR}/pianoforge-obs")
        # Installed into a prefix (a package for the system's OBS: /usr; Flathub's extension: /app/plugins/PianoForge,
        # obs/flatpak), where OBS looks for its plugins: lib/obs-plugins and share/obs/obs-plugins/<name>.
        include(GNUInstallDirs)
        install(TARGETS pianoforge-obs LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/obs-plugins" COMPONENT obs)
        install(DIRECTORY data/ DESTINATION "${CMAKE_INSTALL_DATADIR}/obs/obs-plugins/pianoforge-obs" COMPONENT obs)
        add_custom_target(install-obs-plugin
            COMMAND ${CMAKE_COMMAND} -DDLL=$<TARGET_FILE:pianoforge-obs>
                    -DLOCALES=${CMAKE_CURRENT_SOURCE_DIR}/data/locale -DDEST=${plugin_root}
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_plugin.cmake
            DEPENDS pianoforge-obs
            COMMENT "Installing the PianoForge OBS plugin into ${plugin_root}")
    endif()
endif()

# The plugin inside the installed libobs, without the OBS application (obs/tests/plugin_test.cpp): audio, and video
# through OBS's Direct3D 11 module (offscreen), with OBS's text source loaded beside it.
if(WIN32 AND BUILD_TESTING)
    add_executable(pianoforge-obs-test tests/plugin_test.cpp ${pianoforge_root}/engine/src/audio_feed.cpp
                                      ${pianoforge_root}/engine/src/look_channel.cpp)
    target_include_directories(pianoforge-obs-test PRIVATE ${pianoforge_root}/engine/include)
    target_include_directories(pianoforge-obs-test SYSTEM PRIVATE ${obs_includes})
    target_link_libraries(pianoforge-obs-test PRIVATE "${obs_lib}")
    target_compile_features(pianoforge-obs-test PRIVATE cxx_std_20)
    target_compile_options(pianoforge-obs-test PRIVATE /utf-8 /W3)
    target_compile_definitions(pianoforge-obs-test PRIVATE _CRT_SECURE_NO_WARNINGS)
    add_dependencies(pianoforge-obs-test pianoforge-obs)
    add_test(NAME obs_plugin COMMAND pianoforge-obs-test $<TARGET_FILE:pianoforge-obs> ${CMAKE_CURRENT_SOURCE_DIR}/data
                                     ${CMAKE_CURRENT_BINARY_DIR}/obs-plugin-test.txt ${PIANOFORGE_OBS_DIR})
    file(TO_NATIVE_PATH "${PIANOFORGE_OBS_DIR}/bin/64bit" obs_bin)
    set_tests_properties(obs_plugin PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${obs_bin}")
elseif(UNIX AND NOT APPLE AND BUILD_TESTING)
    # Linux: video through OBS's OpenGL module, on the session's display (Wayland's, or X11's). Inside a Flatpak OBS:
    #   flatpak run --devel --command=sh com.obsproject.Studio -c "cmake -S obs -B build/obs-flatpak -G Ninja \
    #       -DCMAKE_PREFIX_PATH=/app && cmake --build build/obs-flatpak && ctest --test-dir build/obs-flatpak"
    if(EXISTS "/app/share/obs")
        set(obs_prefix_default "/app")
    else()
        set(obs_prefix_default "/usr")
    endif()
    set(PIANOFORGE_OBS_PREFIX "${obs_prefix_default}" CACHE PATH "OBS's prefix (its lib/ and share/obs): /usr, or /app in a Flatpak")
    add_executable(pianoforge-obs-test tests/plugin_test.cpp ${pianoforge_root}/engine/src/audio_feed.cpp
                                      ${pianoforge_root}/engine/src/look_channel.cpp)
    target_include_directories(pianoforge-obs-test PRIVATE ${pianoforge_root}/engine/include)
    target_link_libraries(pianoforge-obs-test PRIVATE OBS::libobs ${CMAKE_DL_LIBS})
    target_compile_features(pianoforge-obs-test PRIVATE cxx_std_20)
    add_dependencies(pianoforge-obs-test pianoforge-obs)
    add_test(NAME obs_plugin COMMAND pianoforge-obs-test $<TARGET_FILE:pianoforge-obs> ${CMAKE_CURRENT_SOURCE_DIR}/data
                                     ${CMAKE_CURRENT_BINARY_DIR}/obs-plugin-test.txt ${PIANOFORGE_OBS_PREFIX})
endif()
