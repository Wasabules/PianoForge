# Copies the PianoForge.vst3 bundle into a VST3 folder, the host running or not (cmake -P, with BINARY, the plugin's
# binary inside the bundle, and DEST defined). Windows keeps a loaded DLL from being overwritten, not from being
# renamed: the one a host uses moves aside (.old, removed by a later install once the host has let go of it) and
# the new one takes its place, for the next time the host loads it.
cmake_minimum_required(VERSION 3.25)

get_filename_component(arch_dir "${BINARY}" DIRECTORY)      # .../PianoForge.vst3/Contents/x86_64-win
get_filename_component(contents "${arch_dir}" DIRECTORY)    # .../PianoForge.vst3/Contents
get_filename_component(bundle "${contents}" DIRECTORY)      # .../PianoForge.vst3
get_filename_component(arch "${arch_dir}" NAME)
get_filename_component(name "${BINARY}" NAME)
set(target_bundle "${DEST}/PianoForge.vst3")
set(target_dir "${target_bundle}/Contents/${arch}")
set(target "${target_dir}/${name}")

file(MAKE_DIRECTORY "${target_dir}")
file(GLOB stale "${target_dir}/*.old")
foreach(file IN LISTS stale)
    execute_process(COMMAND "${CMAKE_COMMAND}" -E rm -f "${file}" RESULT_VARIABLE ignored ERROR_QUIET)
endforeach()

# Everything but the binary (moduleinfo.json, the icon...), then the binary.
file(GLOB_RECURSE files RELATIVE "${bundle}" "${bundle}/*")
foreach(file IN LISTS files)
    if(NOT file STREQUAL "Contents/${arch}/${name}")
        get_filename_component(dir "${target_bundle}/${file}" DIRECTORY)
        file(MAKE_DIRECTORY "${dir}")
        file(COPY_FILE "${bundle}/${file}" "${target_bundle}/${file}" ONLY_IF_DIFFERENT)
    endif()
endforeach()

if(EXISTS "${target}")
    execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files "${BINARY}" "${target}" RESULT_VARIABLE different)
    if(different EQUAL 0)
        message(STATUS "PianoForge.vst3 in ${DEST} is up to date")
        return()
    endif()
    execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${BINARY}" "${target}" RESULT_VARIABLE copied ERROR_QUIET)
    if(copied EQUAL 0)
        message(STATUS "Installed PianoForge.vst3 into ${DEST}")
        return()
    endif()
    string(TIMESTAMP stamp "%Y%m%d-%H%M%S")
    file(RENAME "${target}" "${target}.${stamp}.old")
    file(COPY_FILE "${BINARY}" "${target}")
    message(STATUS "A host is using PianoForge.vst3: the new one is in place, and loads the next time the host loads it")
else()
    file(COPY_FILE "${BINARY}" "${target}")
    message(STATUS "Installed PianoForge.vst3 into ${DEST} (rescan the plugins in the host)")
endif()
