# Shell: standalone application (JUCE). Starts as a listening lab for the reference corpus.
juce_add_gui_app(PianoForgeApp
    PRODUCT_NAME "PianoForge"
    COMPANY_NAME "PianoForge"
    ICON_BIG "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon-512.png"     # from resources/logo.svg
    ICON_SMALL "${CMAKE_CURRENT_SOURCE_DIR}/resources/icon-32.png"
    VERSION ${PROJECT_VERSION})

target_sources(PianoForgeApp PRIVATE
    src/Main.cpp
    src/SampleBank.cpp
    src/ReferencePlayer.cpp
    src/TakeRecorder.cpp
    src/LiveRecorder.cpp
    src/AudioExport.cpp
    src/Mp3Encoder.cpp
    src/MirrorOutput.cpp
    src/MidiFilePlayer.cpp
    src/ui/Meters.cpp
    src/ui/EnginePanel.cpp
    src/ui/TonePanel.cpp
    src/ui/AudioPanel.cpp
    src/ui/PerformancePanel.cpp
    src/ui/PlayerBar.cpp
    src/ui/RoomPanel.cpp
    src/ui/GesturesPanel.cpp
    src/ui/StreamLookPanel.cpp
    src/StreamLook.cpp
    src/ui/TakesPanel.cpp
    src/UpdateChecker.cpp
    src/MainComponent.cpp
    src/VelocityCurveEditor.cpp
    src/SelfTest.cpp
    src/Diagnostics.cpp
    src/OfflineRender.cpp
    src/EngineSwitch.h)

target_compile_definitions(PianoForgeApp PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_FLAC=1
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=1
    JUCE_ALSA=1
    PIANOFORGE_VERSION="${PROJECT_VERSION}")

target_compile_options(PianoForgeApp PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# ASIO (Windows): Steinberg's SDK is fetched at configure time and checked against its hash. Since October
# 2025 it is dual-licensed, proprietary or GPLv3; the GPLv3 option is compatible with JUCE's AGPLv3.
if(WIN32)
    option(PIANOFORGE_ASIO "Enable ASIO drivers (downloads the Steinberg ASIO SDK, GPLv3 option)" ON)
    if(PIANOFORGE_ASIO)
        FetchContent_Declare(asiosdk
            URL      https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip
            URL_HASH SHA256=d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(asiosdk)
        target_include_directories(PianoForgeApp PRIVATE "${asiosdk_SOURCE_DIR}/common")
        target_compile_definitions(PianoForgeApp PRIVATE JUCE_ASIO=1)
    endif()
endif()

target_link_libraries(PianoForgeApp
    PRIVATE
        pianoforge::engine
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_audio_devices
        juce::juce_dsp
        juce::juce_cryptography
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(WIN32) # Media Foundation: the MP3 encoder
    target_link_libraries(PianoForgeApp PRIVATE mf mfplat mfreadwrite mfuuid ole32)
endif()
