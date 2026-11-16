# The VST3 instrument (JUCE; an Audio Unit too on macOS): the application's modal engine, room, headphones and
# limiter in a host.
#   cmake --build --preset windows-release --target install-vst3-plugin
# builds it and copies PianoForge.vst3 to the per-user VST3 folder (Windows: %LOCALAPPDATA%/Programs/Common/VST3,
# which hosts such as REAPER scan; Linux: ~/.vst3; macOS: ~/Library/Audio/Plug-Ins/VST3), no administrator rights
# needed, the host running or not: a plugin in use moves aside and the new one loads when the host loads it again.
# The modal profile is never in the plugin.

juce_add_binary_data(PianoForgePluginData SOURCES ${PROJECT_SOURCE_DIR}/app/resources/logo.svg)

set(pianoforge_formats VST3)
if(APPLE)
    list(APPEND pianoforge_formats AU) # Logic and GarageBand take Audio Units only
endif()
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND pianoforge_formats LV2) # Ardour, Carla, Qtractor, Zrythm prefer LV2
endif()

juce_add_plugin(PianoForgePlugin
    PRODUCT_NAME "PianoForge"
    COMPANY_NAME "PianoForge"
    BUNDLE_ID "org.pianoforge.plugin"
    PLUGIN_MANUFACTURER_CODE Pfge
    PLUGIN_CODE Pfpn
    FORMATS ${pianoforge_formats}
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Instrument Piano
    LV2URI "urn:pianoforge:piano"
    LV2_SHARED_LIBRARY_NAME PianoForge
    ICON_BIG "${PROJECT_SOURCE_DIR}/app/resources/icon-512.png"
    VERSION ${PROJECT_VERSION})

target_sources(PianoForgePlugin PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp)

target_compile_definitions(PianoForgePlugin PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_compile_options(PianoForgePlugin PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

target_link_libraries(PianoForgePlugin
    PRIVATE
        pianoforge::engine
        PianoForgePluginData
        juce::juce_audio_utils
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

if(WIN32)
    file(TO_CMAKE_PATH "$ENV{LOCALAPPDATA}" local_app_data)
    set(vst3_default "${local_app_data}/Programs/Common/VST3")
elseif(APPLE)
    set(vst3_default "$ENV{HOME}/Library/Audio/Plug-Ins/VST3")
else()
    set(vst3_default "$ENV{HOME}/.vst3")
endif()
set(PIANOFORGE_VST3_DIR "${vst3_default}" CACHE PATH "Where install-vst3-plugin copies PianoForge.vst3 (the per-user VST3 folder)")
add_custom_target(install-vst3-plugin
    COMMAND ${CMAKE_COMMAND} -DBINARY=$<TARGET_FILE:PianoForgePlugin_VST3> -DDEST=${PIANOFORGE_VST3_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/install_vst3.cmake
    DEPENDS PianoForgePlugin_VST3
    COMMENT "Installing PianoForge.vst3 into ${PIANOFORGE_VST3_DIR}")

# Linux: the bundles in a prefix's lib/vst3 and lib/lv2 (/usr/lib/... from a package, where hosts look), the components
# vst3 and lv2; and install-lv2-plugin, as install-vst3-plugin, into the per-user folder (~/.lv2).
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    get_target_property(vst3_bundle PianoForgePlugin_VST3 JUCE_PLUGIN_ARTEFACT_FILE)
    install(DIRECTORY "${vst3_bundle}" DESTINATION lib/vst3 USE_SOURCE_PERMISSIONS COMPONENT vst3)
    get_target_property(lv2_bundle PianoForgePlugin_LV2 JUCE_PLUGIN_ARTEFACT_FILE)
    install(DIRECTORY "${lv2_bundle}" DESTINATION lib/lv2 USE_SOURCE_PERMISSIONS COMPONENT lv2)
    set(PIANOFORGE_LV2_DIR "$ENV{HOME}/.lv2" CACHE PATH "Where install-lv2-plugin copies PianoForge.lv2 (the per-user LV2 folder)")
    add_custom_target(install-lv2-plugin
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${PIANOFORGE_LV2_DIR}/PianoForge.lv2"
        COMMAND ${CMAKE_COMMAND} -E copy_directory "${lv2_bundle}" "${PIANOFORGE_LV2_DIR}/PianoForge.lv2"
        DEPENDS PianoForgePlugin_LV2
        COMMENT "Installing PianoForge.lv2 into ${PIANOFORGE_LV2_DIR}")
endif()

# The plugin as a host loads it (plugin/tests/host_test.cpp): ctest plugin_host.
if(BUILD_TESTING)
    juce_add_console_app(PianoForgePluginTest PRODUCT_NAME "PianoForgePluginTest")
    target_sources(PianoForgePluginTest PRIVATE tests/host_test.cpp)
    target_compile_definitions(PianoForgePluginTest PRIVATE
        JUCE_PLUGINHOST_VST3=1
        JUCE_MODAL_LOOPS_PERMITTED=1 # the test pumps the message loop while the profile loads
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)
    target_compile_options(PianoForgePluginTest PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
    target_link_libraries(PianoForgePluginTest
        PRIVATE
            pianoforge::engine
            juce::juce_audio_processors
            juce::juce_gui_basics
        PUBLIC
            juce::juce_recommended_config_flags
            juce::juce_recommended_warning_flags)
    add_dependencies(PianoForgePluginTest PianoForgePlugin_VST3)
    # The bundle is two folders up from the binary (PianoForge.vst3/Contents/x86_64-win/PianoForge.vst3). The sound
    # tests use the reference corpus's profile when it is there (never in the repository).
    set(profile "${PROJECT_SOURCE_DIR}/corpus/reference/noire-dry/modal-profile.bin")
    add_test(NAME plugin_host
             COMMAND PianoForgePluginTest "$<TARGET_FILE_DIR:PianoForgePlugin_VST3>/../.." "${profile}")
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux") # the LV2 plugin, through JUCE's LV2 host: the same checks
        target_compile_definitions(PianoForgePluginTest PRIVATE JUCE_PLUGINHOST_LV2=1)
        add_dependencies(PianoForgePluginTest PianoForgePlugin_LV2)
        add_test(NAME plugin_host_lv2 COMMAND PianoForgePluginTest "${lv2_bundle}" "${profile}")
        # LV2's specifications (the plugin classes: an instrument is a generator), from JUCE's copy, as the bundles a
        # system's lv2 package installs (JUCE's host looks only at folders ending in .lv2): a system without them works.
        set(lv2_sdk "${juce_SOURCE_DIR}/modules/juce_audio_processors_headless/format_types/LV2_SDK/lv2/lv2")
        file(GLOB lv2_specs LIST_DIRECTORIES true "${lv2_sdk}/*")
        foreach(spec IN LISTS lv2_specs)
            if(IS_DIRECTORY "${spec}" AND EXISTS "${spec}/manifest.ttl")
                get_filename_component(spec_name "${spec}" NAME)
                file(COPY "${spec}/" DESTINATION "${CMAKE_CURRENT_BINARY_DIR}/lv2-specs/${spec_name}.lv2")
            endif()
        endforeach()
        set_tests_properties(plugin_host_lv2 PROPERTIES ENVIRONMENT "LV2_PATH=${CMAKE_CURRENT_BINARY_DIR}/lv2-specs")
    endif()
endif()
