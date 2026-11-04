# PianoForge core: pure C++20, no framework.
add_library(pianoforge_engine INTERFACE)
add_library(pianoforge::engine ALIAS pianoforge_engine)
target_include_directories(pianoforge_engine INTERFACE include)
target_compile_features(pianoforge_engine INTERFACE cxx_std_20)

if(BUILD_TESTING)
    add_executable(velocity_curve_test tests/velocity_curve_test.cpp)
    target_link_libraries(velocity_curve_test PRIVATE pianoforge::engine)
    add_test(NAME velocity_curve COMMAND velocity_curve_test)
endif()
