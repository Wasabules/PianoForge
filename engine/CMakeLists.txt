# PianoForge core: pure C++20, no framework.
add_library(pianoforge_engine STATIC
    src/modal_profile.cpp
    src/modal_engine.cpp
    src/midi_sequence.cpp
    src/room.cpp)
# Not in a browser: the profile from a file, and the OBS plugin's shared memory.
if(NOT EMSCRIPTEN)
    target_sources(pianoforge_engine PRIVATE src/modal_profile_file.cpp src/audio_feed.cpp)
endif()
add_library(pianoforge::engine ALIAS pianoforge_engine)
target_include_directories(pianoforge_engine PUBLIC include)
target_compile_features(pianoforge_engine PUBLIC cxx_std_20)

# The resonator banks are written for the compiler to vectorise 8 lanes at a time: AVX2 on x86-64 (every
# x86-64 CPU since 2013-2015).
option(PIANOFORGE_AVX2 "Build the engine for AVX2 (x86-64)" ON)
if(EMSCRIPTEN)
    # WebAssembly: 128-bit SIMD (every current browser); the 8-lane loops vectorise as two 4-lane operations.
    target_compile_options(pianoforge_engine PUBLIC -msimd128 -O3)
elseif(PIANOFORGE_AVX2 AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
    if(MSVC)
        target_compile_options(pianoforge_engine PUBLIC /arch:AVX2)
    else()
        target_compile_options(pianoforge_engine PUBLIC -mavx2 -mfma)
    endif()
endif()
if(MSVC)
    target_compile_options(pianoforge_engine PRIVATE /W4 /utf-8)
else()
    target_compile_options(pianoforge_engine PRIVATE -Wall -Wextra)
endif()

if(NOT EMSCRIPTEN)
    add_executable(modal_render tools/modal_render.cpp)
    target_link_libraries(modal_render PRIVATE pianoforge::engine)
endif()

if(BUILD_TESTING)
    add_executable(velocity_curve_test tests/velocity_curve_test.cpp)
    target_link_libraries(velocity_curve_test PRIVATE pianoforge::engine)
    add_test(NAME velocity_curve COMMAND velocity_curve_test)
    add_executable(mode_bank_test tests/mode_bank_test.cpp)
    target_link_libraries(mode_bank_test PRIVATE pianoforge::engine)
    add_test(NAME mode_bank COMMAND mode_bank_test)
    add_executable(modal_engine_test tests/modal_engine_test.cpp)
    target_link_libraries(modal_engine_test PRIVATE pianoforge::engine)
    add_test(NAME modal_engine COMMAND modal_engine_test)
    if(NOT EMSCRIPTEN)
        add_executable(audio_feed_test tests/audio_feed_test.cpp)
        target_link_libraries(audio_feed_test PRIVATE pianoforge::engine)
        add_test(NAME audio_feed COMMAND audio_feed_test)
    endif()
    add_executable(room_test tests/room_test.cpp)
    target_link_libraries(room_test PRIVATE pianoforge::engine)
    add_test(NAME room COMMAND room_test)
    add_executable(sequence_test tests/sequence_test.cpp)
    target_link_libraries(sequence_test PRIVATE pianoforge::engine)
    add_test(NAME sequence COMMAND sequence_test)
    if(EMSCRIPTEN) # run by Node: a growing memory, and a stack for the engines the tests build on it
        foreach(test velocity_curve_test mode_bank_test modal_engine_test room_test sequence_test)
            target_link_options(${test} PRIVATE -sALLOW_MEMORY_GROWTH=1 -sMAXIMUM_MEMORY=2GB -sSTACK_SIZE=8MB)
        endforeach()
    endif()
endif()
