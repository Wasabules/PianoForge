# The forge's native kernels (C++, no dependency), loaded by tools/forge/forge/native.py when built: each build copies
# the library next to the forge package (tools/forge/forge/_native/, not committed). Part of the project's build, or
# on its own (the CI's forge job):
#   cmake -S tools/forge/native -B build/forge-native -DCMAKE_BUILD_TYPE=Release && cmake --build build/forge-native --config Release
cmake_minimum_required(VERSION 3.25)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(PianoForgeForgeNative LANGUAGES CXX)
    set(CMAKE_CXX_STANDARD 20)
    set(CMAKE_CXX_STANDARD_REQUIRED ON)
endif()

add_library(pianoforge_forge_native SHARED vp_refine.cpp resonators.cpp synth.cpp)
set_target_properties(pianoforge_forge_native PROPERTIES CXX_VISIBILITY_PRESET hidden)
# x86-64: AVX2 (the resonators' eight lanes); every machine the bench runs on has it (the engine needs it too).
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" forge_native_cpu)
set(forge_native_x86 OFF)
if(forge_native_cpu MATCHES "amd64|x86_64|x64")
    set(forge_native_x86 ON)
endif()
if(MSVC)
    target_compile_options(pianoforge_forge_native PRIVATE /W4 $<$<CONFIG:Release>:/O2> $<$<BOOL:${forge_native_x86}>:/arch:AVX2>)
else()
    target_compile_options(pianoforge_forge_native PRIVATE -Wall -Wextra $<$<CONFIG:Release>:-O3>
                           $<$<BOOL:${forge_native_x86}>:-mavx2 -mfma>)
endif()

set(forge_native_dir "${CMAKE_CURRENT_SOURCE_DIR}/../forge/_native")
add_custom_command(TARGET pianoforge_forge_native POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${forge_native_dir}"
    COMMAND ${CMAKE_COMMAND} -E copy $<TARGET_FILE:pianoforge_forge_native> "${forge_native_dir}/"
    COMMENT "Copying the forge's native kernels next to the forge package")
