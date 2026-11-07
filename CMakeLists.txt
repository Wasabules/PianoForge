cmake_minimum_required(VERSION 3.25)

project(PianoForge VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(CTest) # BUILD_TESTING, on by default

# WebAssembly (emcmake): the engine, its tests (run by Node) and the browser module (web/); no JUCE, no app.
if(EMSCRIPTEN)
    add_subdirectory(engine)
    add_subdirectory(web)
    return()
endif()

# JUCE is only used by the shell (app/). The engine (engine/) does not depend on it.
include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        9.0.5
    GIT_SHALLOW    TRUE
    GIT_PROGRESS   TRUE)
FetchContent_MakeAvailable(JUCE)

add_subdirectory(engine)
add_subdirectory(app)

# The VST3 instrument (plugin/): the same engine, room and limiter in a host.
option(PIANOFORGE_PLUGIN "Build the VST3 plugin" ON)
if(PIANOFORGE_PLUGIN)
    add_subdirectory(plugin)
endif()

# The OBS plugin (obs/): on Windows when OBS Studio is installed, on Linux when libobs is.
if(WIN32)
    set(obs_found_default OFF)
    if(EXISTS "C:/Program Files/obs-studio/bin/64bit/obs.dll")
        set(obs_found_default ON)
    endif()
else()
    find_package(libobs QUIET)
    set(obs_found_default ${libobs_FOUND})
endif()
option(PIANOFORGE_OBS_PLUGIN "Build the OBS Studio plugin (the PianoForge audio source)" ${obs_found_default})
if(PIANOFORGE_OBS_PLUGIN)
    add_subdirectory(obs)
endif()

# The Windows installer (installer/): cmake --build --preset windows-release --target installer (Inno Setup 6).
if(WIN32)
    add_subdirectory(installer)
endif()
