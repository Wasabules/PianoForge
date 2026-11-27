# SPDX-License-Identifier: AGPL-3.0-or-later

# Copies the plugin into OBS's plugin folder (Windows, Linux), OBS running or not (cmake -P, with DLL, LOCALES and
# DEST defined): the
# DLL, the locales, and the shaders beside the locales' folder.
# Windows keeps a loaded DLL from being overwritten, not from being renamed: the one OBS uses moves aside (.old,
# removed by a later install once OBS has let go of it) and the new one takes its place, for OBS's next start.
cmake_minimum_required(VERSION 3.25)

set(bin "${DEST}/bin/64bit")
file(MAKE_DIRECTORY "${bin}" "${DEST}/data/locale")
file(GLOB stale "${bin}/*.old")
foreach(file IN LISTS stale)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -f "${file}" RESULT_VARIABLE ignored ERROR_QUIET)
endforeach()
file(GLOB locales "${LOCALES}/*.ini")
foreach(locale IN LISTS locales)
    get_filename_component(name "${locale}" NAME)
    file(COPY_FILE "${locale}" "${DEST}/data/locale/${name}" ONLY_IF_DIFFERENT)
endforeach()
# The shaders (the keyboard's drawing), beside the locales' folder.
get_filename_component(data "${LOCALES}" DIRECTORY)
file(GLOB effects "${data}/*.effect")
foreach(effect IN LISTS effects)
    get_filename_component(name "${effect}" NAME)
    file(COPY_FILE "${effect}" "${DEST}/data/${name}" ONLY_IF_DIFFERENT)
endforeach()

get_filename_component(module "${DLL}" NAME) # pianoforge-obs.dll, pianoforge-obs.so
set(target "${bin}/${module}")
if(EXISTS "${target}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${DLL}" "${target}" RESULT_VARIABLE different)
    if(different EQUAL 0)
        message(STATUS "The PianoForge OBS plugin in ${bin} is up to date")
        return()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${DLL}" "${target}" RESULT_VARIABLE copied ERROR_QUIET)
    if(copied EQUAL 0)
        message(STATUS "Installed the PianoForge OBS plugin into ${bin} (restart OBS to load it)")
        return()
    endif()
    string(TIMESTAMP stamp "%Y%m%d-%H%M%S")
    file(RENAME "${target}" "${target}.${stamp}.old")
    file(COPY_FILE "${DLL}" "${target}")
    message(STATUS "OBS is using the plugin: the new one is in place, and OBS loads it when it starts again")
else()
    file(COPY_FILE "${DLL}" "${target}")
    message(STATUS "Installed the PianoForge OBS plugin into ${bin} (restart OBS to load it)")
endif()
