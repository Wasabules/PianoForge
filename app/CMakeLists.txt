# Shell: standalone application (JUCE). Starts as a listening lab for the reference corpus.
juce_add_gui_app(PianoForgeApp
    PRODUCT_NAME "PianoForge"
    COMPANY_NAME "PianoForge"
    VERSION ${PROJECT_VERSION})

target_sources(PianoForgeApp PRIVATE
    src/Main.cpp
    src/SampleBank.cpp
    src/ReferencePlayer.cpp
    src/MainComponent.cpp
    src/VelocityCurveEditor.cpp
    src/SelfTest.cpp)

target_compile_definitions(PianoForgeApp PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_FLAC=1
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=1
    JUCE_ALSA=1
    PIANOFORGE_VERSION="${PROJECT_VERSION}")

target_compile_options(PianoForgeApp PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

target_link_libraries(PianoForgeApp
    PRIVATE
        pianoforge::engine
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_audio_devices
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
