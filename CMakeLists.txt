cmake_minimum_required(VERSION 3.22)

project(Secant VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")

add_subdirectory(third_party/JUCE)

juce_add_gui_app(Secant
    PRODUCT_NAME "Secant"
    COMPANY_NAME "Summit"
    BUNDLE_ID "com.summit.secant")

target_sources(Secant PRIVATE
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/PresetModel.cpp
    Source/OnsetAnalyzer.cpp
    Source/LegatoAnalyzer.cpp
    Source/SineState.cpp
    Source/Planning.cpp
    Source/RenderWorker.cpp
    Source/ResultStore.cpp
    Source/DelayExport.cpp)

target_compile_definitions(Secant PRIVATE
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_AU=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Secant,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Secant,JUCE_VERSION>")

target_link_libraries(Secant PRIVATE
    juce::juce_audio_utils
    juce::juce_audio_processors
    juce::juce_dsp
    PUBLIC
    juce::juce_recommended_config_flags
    juce::juce_recommended_lto_flags
    juce::juce_recommended_warning_flags)

# Unit test for the onset analyzer (no plugin needed): synthesises attacks with
# known onset times and checks the sub-sample estimates.
juce_add_console_app(OnsetAnalyzerTest PRODUCT_NAME "OnsetAnalyzerTest")
target_sources(OnsetAnalyzerTest PRIVATE Tests/OnsetAnalyzerTest.cpp Source/OnsetAnalyzer.cpp Source/LegatoAnalyzer.cpp Source/PresetModel.cpp)
target_compile_definitions(OnsetAnalyzerTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(OnsetAnalyzerTest PRIVATE juce::juce_core juce::juce_audio_basics juce::juce_dsp
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Headless end-to-end test: real VST3 -> cloned RenderWorkers -> results JSON.
juce_add_console_app(PipelineSmokeTest PRODUCT_NAME "PipelineSmokeTest")
target_sources(PipelineSmokeTest PRIVATE Tests/PipelineSmokeTest.cpp
    Source/RenderWorker.cpp Source/ResultStore.cpp Source/OnsetAnalyzer.cpp Source/PresetModel.cpp
    Source/LegatoAnalyzer.cpp Source/SineState.cpp Source/Planning.cpp)
target_compile_definitions(PipelineSmokeTest PRIVATE
    JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
    JUCE_APPLICATION_NAME_STRING="PipelineSmokeTest" JUCE_APPLICATION_VERSION_STRING="0.2.0")
target_link_libraries(PipelineSmokeTest PRIVATE juce::juce_audio_processors juce::juce_dsp juce::juce_gui_extra
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Round-robin behaviour probe: VST3 + saved .pluginstate -> sample identity sequence at several note spacings.
juce_add_console_app(RRProbe PRODUCT_NAME "RRProbe")
target_sources(RRProbe PRIVATE Tests/RRProbe.cpp Source/OnsetAnalyzer.cpp Source/PresetModel.cpp)
target_compile_definitions(RRProbe PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(RRProbe PRIVATE juce::juce_audio_processors juce::juce_dsp juce::juce_gui_extra
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Legato behaviour probe: VST3 + saved .pluginstate -> transition timing per velocity / note gap.
juce_add_console_app(LegatoProbe PRODUCT_NAME "LegatoProbe")
target_sources(LegatoProbe PRIVATE Tests/LegatoProbe.cpp Source/LegatoAnalyzer.cpp Source/OnsetAnalyzer.cpp Source/PresetModel.cpp)
target_compile_definitions(LegatoProbe PRIVATE JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_AU=0 JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(LegatoProbe PRIVATE juce::juce_audio_processors juce::juce_dsp juce::juce_gui_extra
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# Delays export from saved results files: DelayExport <results.json>... -> <name>.delays.json, <name>.expressionmap
juce_add_console_app(DelayExport PRODUCT_NAME "DelayExport")
target_sources(DelayExport PRIVATE Tests/DelayExportMain.cpp Source/DelayExport.cpp Source/PresetModel.cpp)
target_compile_definitions(DelayExport PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(DelayExport PRIVATE juce::juce_core
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)

# SINE state inspection / RR-reset patch check: SineStateTool <state> [--json | --rr-reset <ms>]
juce_add_console_app(SineStateTool PRODUCT_NAME "SineStateTool")
target_sources(SineStateTool PRIVATE Tests/SineStateTool.cpp Source/SineState.cpp)
target_compile_definitions(SineStateTool PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0)
target_link_libraries(SineStateTool PRIVATE juce::juce_core
    PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
