# The forge monitor (JUCE): a small window that follows the forge pipelines of a corpus (their progress.json, written
# by forge pipeline): each one's steps, the one under way and its progress, the time left, the end of its log.
#   cmake --build --preset windows-release --target PianoForgeForgeMonitor
#   build/windows-msvc/tools/forge/monitor/PianoForgeForgeMonitor_artefacts/Release/PianoForgeForgeMonitor.exe [corpus]
juce_add_gui_app(PianoForgeForgeMonitor
    PRODUCT_NAME "PianoForgeForgeMonitor"
    COMPANY_NAME "PianoForge"
    ICON_BIG "${PROJECT_SOURCE_DIR}/app/resources/icon-512.png"
    ICON_SMALL "${PROJECT_SOURCE_DIR}/app/resources/icon-32.png"
    VERSION ${PROJECT_VERSION})

target_sources(PianoForgeForgeMonitor PRIVATE Main.cpp)
target_compile_definitions(PianoForgeForgeMonitor PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_compile_options(PianoForgeForgeMonitor PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/utf-8>)
target_link_libraries(PianoForgeForgeMonitor
    PRIVATE
        juce::juce_gui_basics
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
