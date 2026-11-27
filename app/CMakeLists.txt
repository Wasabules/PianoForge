# SPDX-License-Identifier: AGPL-3.0-or-later

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
    src/LinuxAudio.cpp
    src/PipeWireAudio.cpp
    src/AudioDevices.cpp
    src/DesktopBus.cpp
    src/DesktopIntegration.cpp
    src/SingleInstance.cpp
    src/ObsFlatpak.cpp
    src/ui/DesktopPanel.cpp
    src/AudioExport.cpp
    src/Mp3Encoder.cpp
    src/MirrorOutput.cpp
    src/MidiFilePlayer.cpp
    src/ui/Meters.cpp
    src/ui/EnginePanel.cpp
    src/ui/TonePanel.cpp
    src/ui/SettingsPanel.cpp
    src/ui/AudioPanel.cpp
    src/ui/PerformancePanel.cpp
    src/ui/PlayerBar.cpp
    src/ui/RoomPanel.cpp
    src/ui/GesturesPanel.cpp
    src/ui/StreamLookPanel.cpp
    src/StreamLook.cpp
    src/ui/GalleryPanel.cpp
    src/ui/LevelStrip.cpp
    src/ui/NamingDialog.cpp
    src/ui/RecordingPanel.cpp
    src/Gallery.cpp
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

# JACK and PipeWire (Linux): JUCE's JACK device type, which plays through PipeWire where its JACK library is installed
# (src/LinuxAudio.cpp), or through a JACK server. JUCE loads libjack at run time, so only its headers are needed here:
# the system's (libjack-jackd2-dev), or else jack2's of the version Debian and Ubuntu ship, fetched and checked against
# their hash.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    option(PIANOFORGE_JACK "Enable JACK audio (PipeWire's JACK, or a JACK server; libjack is loaded at run time)" ON)
    if(PIANOFORGE_JACK)
        find_path(PIANOFORGE_JACK_INCLUDE_DIR jack/jack.h)
        set(jack_include "${PIANOFORGE_JACK_INCLUDE_DIR}")
        if(NOT PIANOFORGE_JACK_INCLUDE_DIR)
            FetchContent_Declare(jack2_headers
                URL      https://github.com/jackaudio/jack2/archive/refs/tags/v1.9.22.tar.gz
                URL_HASH SHA256=1e42b9fc4ad7db7befd414d45ab2f8a159c0b30fcd6eee452be662298766a849
                DOWNLOAD_EXTRACT_TIMESTAMP TRUE
                SOURCE_SUBDIR do-not-build) # the headers only
            FetchContent_MakeAvailable(jack2_headers)
            set(jack_include "${jack2_headers_SOURCE_DIR}/common")
        endif()
        target_include_directories(PianoForgeApp PRIVATE "${jack_include}")
        target_compile_definitions(PianoForgeApp PRIVATE JUCE_JACK=1 JUCE_JACK_CLIENT_NAME="PianoForge")
    endif()

    # PipeWire's own device type (src/PipeWireAudio.cpp). libpipewire is loaded at run time, so only its headers are
    # needed: the system's (libpipewire-0.3-dev), or else PipeWire 1.0.5's (Ubuntu 24.04's: nothing newer is used),
    # fetched and checked against their hash, with the version header PipeWire's own build would write.
    option(PIANOFORGE_PIPEWIRE "PipeWire's own device type (libpipewire loaded at run time)" ON)
    if(PIANOFORGE_PIPEWIRE)
        find_package(PkgConfig QUIET)
        if(PkgConfig_FOUND)
            pkg_check_modules(PIANOFORGE_LIBPIPEWIRE QUIET libpipewire-0.3)
        endif()
        if(PIANOFORGE_LIBPIPEWIRE_FOUND)
            set(pipewire_includes ${PIANOFORGE_LIBPIPEWIRE_INCLUDE_DIRS})
        else()
            FetchContent_Declare(pipewire_headers
                URL      https://gitlab.freedesktop.org/pipewire/pipewire/-/archive/1.0.5/pipewire-1.0.5.tar.gz
                URL_HASH SHA256=c5a5de26d684a1a84060ad7b6131654fb2835e03fccad85059be92f8e3ffe993
                DOWNLOAD_EXTRACT_TIMESTAMP TRUE
                SOURCE_SUBDIR do-not-build) # the headers only
            FetchContent_MakeAvailable(pipewire_headers)
            set(PIPEWIRE_VERSION_MAJOR 1)
            set(PIPEWIRE_VERSION_MINOR 0)
            set(PIPEWIRE_VERSION_MICRO 5)
            set(PIPEWIRE_API_VERSION "\"0.3\"")
            configure_file("${pipewire_headers_SOURCE_DIR}/src/pipewire/version.h.in"
                           "${CMAKE_CURRENT_BINARY_DIR}/pipewire-headers/pipewire/version.h" @ONLY)
            set(pipewire_includes "${CMAKE_CURRENT_BINARY_DIR}/pipewire-headers" "${pipewire_headers_SOURCE_DIR}/src"
                                  "${pipewire_headers_SOURCE_DIR}/spa/include")
        endif()
        target_include_directories(PianoForgeApp SYSTEM PRIVATE ${pipewire_includes})
        target_compile_definitions(PianoForgeApp PRIVATE PIANOFORGE_PIPEWIRE=1)
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

# Linux: the window's icon (the window manager shows it in the task switcher), and an installation into a prefix:
#   cmake --install build/linux-gcc --component pianoforge --prefix ~/.local        (or /usr/local, as root)
# puts `pianoforge` in bin/, its menu entry (opening MIDI files too) and its icons in share/. (The component leaves
# out what JUCE installs for developers: its headers, CMake files and tools.) Settings, takes and
# recordings stay in the user's folders (~/.config/PianoForge, ~/Documents/PianoForge); the corpus is chosen in the
# application (File > Change corpus) or given by PIANOFORGE_CORPUS.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    juce_add_binary_data(PianoForgeAppData NAMESPACE PianoForgeAppData SOURCES resources/logo.svg)
    target_link_libraries(PianoForgeApp PRIVATE PianoForgeAppData)
    target_compile_definitions(PianoForgeApp PRIVATE PIANOFORGE_WINDOW_ICON=1)

    include(GNUInstallDirs)
    install(PROGRAMS "$<TARGET_FILE:PianoForgeApp>" DESTINATION "${CMAKE_INSTALL_BINDIR}" RENAME pianoforge COMPONENT pianoforge)
    # Named after the application's id (org.pianoforge.PianoForge: the desktop entry, its icon, its AppStream metadata,
    # as software centres and Flathub look them up).
    install(FILES linux/org.pianoforge.PianoForge.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications" COMPONENT pianoforge)
    install(FILES linux/org.pianoforge.PianoForge.metainfo.xml DESTINATION "${CMAKE_INSTALL_DATADIR}/metainfo" COMPONENT pianoforge)
    install(FILES resources/logo.svg DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps"
            RENAME org.pianoforge.PianoForge.svg COMPONENT pianoforge)
    install(FILES resources/icon-512.png DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps"
            RENAME org.pianoforge.PianoForge.png COMPONENT pianoforge)
    install(FILES resources/icon-32.png DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/32x32/apps"
            RENAME org.pianoforge.PianoForge.png COMPONENT pianoforge)
    # The licence and the third-party notices where a distribution keeps them (Debian: the copyright file).
    install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/pianoforge" RENAME copyright COMPONENT pianoforge)
    install(FILES "${PROJECT_SOURCE_DIR}/NOTICE.md" DESTINATION "${CMAKE_INSTALL_DATADIR}/doc/pianoforge" COMPONENT pianoforge)
    # The menus' MIME cache ("Open with PianoForge" on MIDI files), where the tool is; a desktop also notices later.
    # Not into a staging folder (DESTDIR: a package being made), whose system's own triggers make the cache.
    install(CODE "
        find_program(update_desktop_database update-desktop-database)
        if(update_desktop_database AND \"\$ENV{DESTDIR}\" STREQUAL \"\")
            execute_process(COMMAND \${update_desktop_database} -q \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_DATADIR}/applications\")
        endif()" COMPONENT pianoforge)
endif()
