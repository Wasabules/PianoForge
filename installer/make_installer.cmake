# Stages what the installer holds, checks it, compiles the installer (Inno Setup) and writes the update manifest next
# to it (cmake -P, with the variables installer/CMakeLists.txt gives).
cmake_minimum_required(VERSION 3.25)

file(REMOVE_RECURSE "${STAGE}")
file(MAKE_DIRECTORY "${STAGE}" "${OUTPUT}")

# The application and the Visual C++ runtime it needs, next to it.
file(COPY_FILE "${EXE}" "${STAGE}/PianoForge.exe")
string(REPLACE "|" ";" runtime "${RUNTIME}")
list(LENGTH runtime count)
if(NOT count EQUAL 3)
    message(FATAL_ERROR "The Visual C++ runtime (msvcp140, vcruntime140, vcruntime140_1) was not found: '${RUNTIME}'")
endif()
foreach(dll IN LISTS runtime)
    get_filename_component(name "${dll}" NAME)
    string(TOLOWER "${name}" name)
    file(COPY_FILE "${dll}" "${STAGE}/${name}")
endforeach()

# The OBS plugin, when it is built.
set(with_obs 0)
if(OBS_DLL AND EXISTS "${OBS_DLL}")
    file(MAKE_DIRECTORY "${STAGE}/obs")
    file(COPY_FILE "${OBS_DLL}" "${STAGE}/obs/pianoforge-obs.dll")
    foreach(language en-US fr-FR)
        file(COPY_FILE "${OBS_LOCALES}/${language}.ini" "${STAGE}/obs/${language}.ini")
    endforeach()
    set(with_obs 1)
endif()

# The VST3 plugin, its whole bundle (PianoForge.vst3/Contents/x86_64-win/PianoForge.vst3 and what is around it).
set(with_vst3 0)
if(VST3_BINARY AND EXISTS "${VST3_BINARY}")
    get_filename_component(arch_dir "${VST3_BINARY}" DIRECTORY)
    get_filename_component(contents "${arch_dir}" DIRECTORY)
    get_filename_component(bundle "${contents}" DIRECTORY)
    file(GLOB_RECURSE bundle_files RELATIVE "${bundle}" "${bundle}/*")
    foreach(file IN LISTS bundle_files)
        get_filename_component(dir "${STAGE}/vst3/PianoForge.vst3/${file}" DIRECTORY)
        file(MAKE_DIRECTORY "${dir}")
        file(COPY_FILE "${bundle}/${file}" "${STAGE}/vst3/PianoForge.vst3/${file}")
    endforeach()
    set(with_vst3 1)
endif()

# Never the corpus nor a profile calibrated on it (rendered from a commercial library: its licence forbids passing
# them on). Only the files above, by name; and the executable carries no data of that size (a profile is tens of MB).
set(allowed PianoForge.exe msvcp140.dll vcruntime140.dll vcruntime140_1.dll obs/pianoforge-obs.dll obs/en-US.ini obs/fr-FR.ini
    vst3/PianoForge.vst3/Contents/x86_64-win/PianoForge.vst3 vst3/PianoForge.vst3/Contents/Resources/moduleinfo.json
    vst3/PianoForge.vst3/desktop.ini vst3/PianoForge.vst3/Plugin.ico)
file(GLOB_RECURSE staged RELATIVE "${STAGE}" "${STAGE}/*")
foreach(file IN LISTS staged)
    if(NOT file IN_LIST allowed OR file MATCHES "corpus|profile|\\.bin$|\\.flac$|\\.wav$")
        message(FATAL_ERROR "Not for the installer: ${file}")
    endif()
endforeach()
if(with_vst3)
    file(SIZE "${STAGE}/vst3/PianoForge.vst3/Contents/x86_64-win/PianoForge.vst3" vst3_size)
    if(vst3_size GREATER 40000000)
        message(FATAL_ERROR "The VST3 plugin is ${vst3_size} bytes: does it carry data it should not?")
    endif()
endif()
file(SIZE "${STAGE}/PianoForge.exe" exe_size)
if(exe_size GREATER 40000000)
    message(FATAL_ERROR "PianoForge.exe is ${exe_size} bytes: does it carry data it should not?")
endif()

if(NOT EXISTS "${ICON}")
    message(FATAL_ERROR "No icon at ${ICON} (made by the application's build)")
endif()
execute_process(COMMAND "${ISCC}" /Q "/DAppVersion=${VERSION}" "/DStageDir=${STAGE}" "/DOutputDir=${OUTPUT}"
                        "/DIconFile=${ICON}" "/DWithObs=${with_obs}" "/DWithVst3=${with_vst3}" "${ISS}"
                RESULT_VARIABLE compiled)
if(NOT compiled EQUAL 0)
    message(FATAL_ERROR "Inno Setup failed (${compiled})")
endif()

# The update manifest (see app/src/UpdateChecker.h) and the SHA-256 on its own (a GitHub release asset).
set(name "PianoForge-${VERSION}-setup.exe")
set(setup "${OUTPUT}/${name}")
file(SHA256 "${setup}" sum)
file(SIZE "${setup}" size)
if(BASE_URL)
    string(REGEX REPLACE "/+$" "" base "${BASE_URL}")
    set(url "${base}/${name}")
else()
    set(url "${name}")
endif()
set(notes "${NOTES}")
string(REPLACE "\\" "\\\\" notes "${notes}")
string(REPLACE "\"" "\\\"" notes "${notes}")
string(REPLACE "\n" "\\n" notes "${notes}")
file(WRITE "${OUTPUT}/latest.json"
     "{\n  \"version\": \"${VERSION}\",\n  \"url\": \"${url}\",\n  \"sha256\": \"${sum}\",\n  \"size\": ${size},\n  \"notes\": \"${notes}\"\n}\n")
file(WRITE "${setup}.sha256" "${sum}  ${name}\n")
math(EXPR megabytes "${size} / 1048576")
message(STATUS "Installer: ${setup} (${megabytes} MB, SHA-256 ${sum}), VST3 plugin ${with_vst3}, OBS plugin ${with_obs}")
message(STATUS "Update manifest: ${OUTPUT}/latest.json")
