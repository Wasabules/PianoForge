# PianoForge core: pure C++20, no framework. For now, only the engine interface.
add_library(pianoforge_engine INTERFACE)
add_library(pianoforge::engine ALIAS pianoforge_engine)
target_include_directories(pianoforge_engine INTERFACE include)
target_compile_features(pianoforge_engine INTERFACE cxx_std_20)
