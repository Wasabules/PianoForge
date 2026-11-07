# The Windows installer (Inno Setup 6):
#   cmake --build --preset windows-release --target installer
# makes build/windows-msvc/installer/output/PianoForge-<version>-setup.exe, the update manifest latest.json and
# the installer's SHA-256 next to it (make_installer.cmake). The installer holds the application, the Visual C++
# runtime it needs (next to it: no administrator rights, no redistributable to run), the VST3 plugin and, when it is
# built, the OBS plugin. Never the corpus nor a profile calibrated on it: the installed application asks for its corpus.

find_program(PIANOFORGE_ISCC ISCC
    HINTS "C:/Program Files (x86)/Inno Setup 6" "C:/Program Files/Inno Setup 6" "$ENV{LOCALAPPDATA}/Programs/Inno Setup 6"
    DOC "Inno Setup 6's command-line compiler (ISCC.exe)")
set(PIANOFORGE_UPDATE_BASE_URL "" CACHE STRING
    "Where the installer will be published (https://.../folder): the manifest's url. Empty: relative, next to the manifest")
set(PIANOFORGE_RELEASE_NOTES "" CACHE STRING "What changed, for the update manifest")

if(NOT PIANOFORGE_ISCC)
    message(STATUS "Inno Setup 6 not found: no installer target")
    return()
endif()

# The Visual C++ runtime the application links to (msvcp140, vcruntime140, vcruntime140_1), from this compiler's
# redistributable folder; the Universal CRT is part of Windows 10 and later.
set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
set(CMAKE_INSTALL_UCRT_LIBRARIES FALSE)
include(InstallRequiredSystemLibraries)
set(runtime "")
foreach(dll IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
    get_filename_component(name "${dll}" NAME)
    string(TOLOWER "${name}" name)
    if(name MATCHES "^(msvcp140|vcruntime140|vcruntime140_1)\\.dll$")
        list(APPEND runtime "${dll}")
    endif()
endforeach()
string(REPLACE ";" "|" runtime_argument "${runtime}")

set(obs_dll "")
if(TARGET pianoforge-obs)
    set(obs_dll "$<TARGET_FILE:pianoforge-obs>")
endif()
set(vst3_binary "")
if(TARGET PianoForgePlugin_VST3)
    set(vst3_binary "$<TARGET_FILE:PianoForgePlugin_VST3>")
endif()

add_custom_target(installer
    COMMAND ${CMAKE_COMMAND}
            -DVERSION=${PROJECT_VERSION}
            -DEXE=$<TARGET_FILE:PianoForgeApp>
            -DRUNTIME=${runtime_argument}
            -DOBS_DLL=${obs_dll}
            -DOBS_LOCALES=${PROJECT_SOURCE_DIR}/obs/data/locale
            -DVST3_BINARY=${vst3_binary}
            -DICON=${CMAKE_BINARY_DIR}/app/PianoForgeApp_artefacts/JuceLibraryCode/icon.ico
            -DISCC=${PIANOFORGE_ISCC}
            -DISS=${CMAKE_CURRENT_SOURCE_DIR}/pianoforge.iss
            -DSTAGE=${CMAKE_CURRENT_BINARY_DIR}/stage
            -DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/output
            -DBASE_URL=${PIANOFORGE_UPDATE_BASE_URL}
            -DNOTES=${PIANOFORGE_RELEASE_NOTES}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/make_installer.cmake
    VERBATIM
    COMMENT "Building the PianoForge installer")
add_dependencies(installer PianoForgeApp)
if(TARGET pianoforge-obs)
    add_dependencies(installer pianoforge-obs)
endif()
if(TARGET PianoForgePlugin_VST3)
    add_dependencies(installer PianoForgePlugin_VST3)
endif()
