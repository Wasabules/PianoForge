# PianoForge core: pure C++20, no framework.
add_library(pianoforge_engine STATIC
    src/modal_profile.cpp
    src/modal_engine.cpp)
add_library(pianoforge::engine ALIAS pianoforge_engine)
target_include_directories(pianoforge_engine PUBLIC include)
target_compile_features(pianoforge_engine PUBLIC cxx_std_20)

# The resonator banks are written for the compiler to vectorise 8 lanes at a time: AVX2 on x86-64 (every
# x86-64 CPU since 2013-2015).
option(PIANOFORGE_AVX2 "Build the engine for AVX2 (x86-64)" ON)
if(PIANOFORGE_AVX2 AND CMAKE_SYSTEM_PROCESSOR MATCHES "AMD64|x86_64")
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

add_executable(modal_render tools/modal_render.cpp)
target_link_libraries(modal_render PRIVATE pianoforge::engine)

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
endif()
