# OBS plugin: the "PianoForge" audio source, fed by the application through shared memory
# (engine/include/pianoforge/audio_feed.hpp). No virtual cable, no system mixer in between.
#
# Windows: built against the installed OBS Studio. The libobs headers of the same version are fetched (and checked
# against their hash); the import library is made from the installed obs.dll's exports. The target
# install-obs-plugin copies the plugin where OBS looks for plugins of every user, no administrator rights needed,
# OBS running or not (the new plugin then loads at its next start):
# %ProgramData%/obs-studio/plugins/pianoforge-obs/bin/64bit.
# Linux: built against the system's libobs (package libobs-dev or obs-studio's development files).

set(PIANOFORGE_OBS_VERSION "33.0.1" CACHE STRING "OBS Studio version the plugin is built against (its libobs headers)")
set(PIANOFORGE_OBS_HASH "9174ee0e3677c92d0d060bdce4c5d44dfee9fd3afddfb6333f098fd029b8de2b" CACHE STRING
    "SHA-256 of the OBS Studio source archive of that version")

add_library(pianoforge-obs MODULE
    src/pianoforge-obs.cpp
    ${PROJECT_SOURCE_DIR}/engine/src/audio_feed.cpp) # built here, without the engine's AVX2 flags
target_include_directories(pianoforge-obs PRIVATE ${PROJECT_SOURCE_DIR}/engine/include)
target_compile_features(pianoforge-obs PRIVATE cxx_std_20)
set_target_properties(pianoforge-obs PROPERTIES PREFIX "" OUTPUT_NAME "pianoforge-obs")

if(WIN32)
    set(PIANOFORGE_OBS_DIR "C:/Program Files/obs-studio" CACHE PATH "OBS Studio installation")
    set(obs_dll "${PIANOFORGE_OBS_DIR}/bin/64bit/obs.dll")

    FetchContent_Declare(obs_sources
        URL      https://github.com/obsproject/obs-studio/archive/refs/tags/${PIANOFORGE_OBS_VERSION}.tar.gz
        URL_HASH SHA256=${PIANOFORGE_OBS_HASH}
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        SOURCE_SUBDIR do-not-build) # the headers only
    FetchContent_MakeAvailable(obs_sources)

    # obs.lib from obs.dll: its exports (dumpbin, next to the linker) become a .def file for lib.exe.
    get_filename_component(msvc_tools "${CMAKE_LINKER}" DIRECTORY)
    find_program(PIANOFORGE_DUMPBIN dumpbin HINTS "${msvc_tools}" REQUIRED)
    set(obs_lib "${CMAKE_CURRENT_BINARY_DIR}/obs.lib")
    if(NOT EXISTS "${obs_lib}" OR "${obs_dll}" IS_NEWER_THAN "${obs_lib}")
        execute_process(COMMAND "${PIANOFORGE_DUMPBIN}" /nologo /exports "${obs_dll}" OUTPUT_VARIABLE exports
                        RESULT_VARIABLE dumped)
        if(NOT dumped EQUAL 0)
            message(FATAL_ERROR "dumpbin could not read ${obs_dll}")
        endif()
        string(REGEX MATCHALL "\n +[0-9]+ +[0-9A-Fa-f]+ +[0-9A-Fa-f]+ +[A-Za-z_][A-Za-z0-9_]*" rows "${exports}")
        set(def "LIBRARY obs\nEXPORTS\n")
        foreach(row IN LISTS rows)
            string(REGEX REPLACE ".* ([A-Za-z_][A-Za-z0-9_]*)$" "\\1" name "${row}")
            string(APPEND def "    ${name}\n")
        endforeach()
        file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/obs.def" "${def}")
        execute_process(COMMAND "${CMAKE_AR}" /nologo /def:${CMAKE_CURRENT_BINARY_DIR}/obs.def /machine:x64
                                /out:${obs_lib} RESULT_VARIABLE made)
        if(NOT made EQUAL 0)
            message(FATAL_ERROR "lib.exe could not make ${obs_lib}")
        endif()
    endif()
    # The header libobs's build generates (install paths, release flags): the values of OBS's Windows build, unused
    # by the plugin.
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/obsconfig.h"
         "#pragma once\n#define OBS_DATA_PATH \"../../data\"\n#define OBS_PLUGIN_PATH \"../../obs-plugins/64bit\"\n"
         "#define OBS_PLUGIN_DESTINATION \"obs-plugins/64bit\"\n#define OBS_RELEASE_CANDIDATE 0\n#define OBS_BETA 0\n")
    target_include_directories(pianoforge-obs SYSTEM PRIVATE "${obs_sources_SOURCE_DIR}/libobs"
                                                              "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(pianoforge-obs PRIVATE "${obs_lib}")
    target_compile_options(pianoforge-obs PRIVATE /utf-8 /W3)

    file(TO_CMAKE_PATH "$ENV{ProgramData}" program_data)
    set(plugin_root "${program_data}/obs-studio/plugins/pianoforge-obs")
    add_custom_target(install-obs-plugin
        COMMAND ${CMAKE_COMMAND} -DDLL=$<TARGET_FILE:pianoforge-obs>
                -DLOCALE=${CMAKE_CURRENT_SOURCE_DIR}/data/locale/en-US.ini -DDEST=${plugin_root}
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_plugin.cmake
        DEPENDS pianoforge-obs
        COMMENT "Installing the PianoForge OBS plugin into ${plugin_root}")
else()
    find_package(libobs REQUIRED)
    target_link_libraries(pianoforge-obs PRIVATE OBS::libobs)
endif()

# The plugin inside the installed libobs, without the OBS application (obs/tests/plugin_test.cpp).
if(WIN32 AND BUILD_TESTING)
    add_executable(pianoforge-obs-test tests/plugin_test.cpp ${PROJECT_SOURCE_DIR}/engine/src/audio_feed.cpp)
    target_include_directories(pianoforge-obs-test PRIVATE ${PROJECT_SOURCE_DIR}/engine/include)
    target_include_directories(pianoforge-obs-test SYSTEM PRIVATE "${obs_sources_SOURCE_DIR}/libobs"
                                                                   "${CMAKE_CURRENT_BINARY_DIR}/generated")
    target_link_libraries(pianoforge-obs-test PRIVATE "${obs_lib}")
    target_compile_options(pianoforge-obs-test PRIVATE /utf-8 /W3)
    target_compile_definitions(pianoforge-obs-test PRIVATE _CRT_SECURE_NO_WARNINGS)
    add_dependencies(pianoforge-obs-test pianoforge-obs)
    add_test(NAME obs_plugin COMMAND pianoforge-obs-test $<TARGET_FILE:pianoforge-obs> obs-plugin-test.txt)
    file(TO_NATIVE_PATH "${PIANOFORGE_OBS_DIR}/bin/64bit" obs_bin)
    set_tests_properties(obs_plugin PROPERTIES ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${obs_bin}")
endif()
