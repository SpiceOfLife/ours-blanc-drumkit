# ours-blanc Drum Kit — Mac / Windows standalone + VST3 / AU (JUCE), sharing engine/ with the R36S and web builds.
#
#   cmake -B build-plugin -DCMAKE_BUILD_TYPE=Release
#   cmake --build build-plugin --config Release
#   ctest --test-dir build-plugin -C Release
#
# DK_PLUGIN=OFF builds only the engine and its tests (no JUCE download).
# The R36S core is built with the Makefile instead.
cmake_minimum_required(VERSION 3.22)
project(OursBlancDrumKit VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "")
if(APPLE)
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "")
endif()

option(DK_PLUGIN "Build the JUCE app and plug-ins" ON)

add_library(dk_engine STATIC engine/dk_engine.c)
set_target_properties(dk_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(dk_engine PUBLIC engine)
if(MSVC)
    target_compile_definitions(dk_engine PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
    target_compile_options(dk_engine PRIVATE /W3 /wd4244)
elseif(NOT WIN32)
    target_link_libraries(dk_engine PUBLIC m)
endif()

enable_testing()
add_executable(engine_test test/engine_test.c)
target_link_libraries(engine_test PRIVATE dk_engine)
add_test(NAME engine_test COMMAND engine_test)

if(NOT DK_PLUGIN)
    return()
endif()

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.9
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(OursBlancDrumKit
    VERSION ${PROJECT_VERSION}
    COMPANY_NAME "ours-blanc"
    COMPANY_WEBSITE "https://ours-blanc.app/lab/drumkit/"
    BUNDLE_ID app.ours-blanc.drumkit
    PRODUCT_NAME "ours-blanc Drum Kit"
    PLUGIN_MANUFACTURER_CODE OBln
    PLUGIN_CODE OBdk
    FORMATS VST3 AU Standalone
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    VST3_CATEGORIES Instrument Drum
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    COPY_PLUGIN_AFTER_BUILD FALSE)

target_sources(OursBlancDrumKit PRIVATE
    plugin/PluginProcessor.cpp
    plugin/PluginEditor.cpp)

target_compile_definitions(OursBlancDrumKit PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(OursBlancDrumKit
    PRIVATE
        dk_engine
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# headless check of the processor (MIDI, host sync, state) — runs in CI on every platform
juce_add_console_app(PluginTest PRODUCT_NAME "PluginTest")
target_sources(PluginTest PRIVATE test/plugin_test.cpp plugin/PluginProcessor.cpp)
target_compile_definitions(PluginTest PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 DK_NO_EDITOR=1
    JucePlugin_Name="ours-blanc Drum Kit" JucePlugin_IsSynth=1 JucePlugin_WantsMidiInput=1
    JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0)
target_link_libraries(PluginTest PRIVATE dk_engine juce::juce_audio_utils juce::juce_recommended_config_flags)
add_test(NAME plugin_test COMMAND PluginTest)

# renders the editor to docs/plugin.png without a window (cmake -DDK_SNAPSHOT=ON)
option(DK_SNAPSHOT "Build the editor screenshot tool" OFF)
if(DK_SNAPSHOT)
    juce_add_console_app(EditorSnapshot PRODUCT_NAME "EditorSnapshot")
    target_sources(EditorSnapshot PRIVATE test/editor_snapshot.cpp plugin/PluginProcessor.cpp plugin/PluginEditor.cpp)
    target_compile_definitions(EditorSnapshot PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0
        JucePlugin_Name="ours-blanc Drum Kit" JucePlugin_IsSynth=1 JucePlugin_WantsMidiInput=1
        JucePlugin_ProducesMidiOutput=0 JucePlugin_IsMidiEffect=0)
    target_link_libraries(EditorSnapshot PRIVATE dk_engine juce::juce_audio_utils juce::juce_recommended_config_flags)
endif()
