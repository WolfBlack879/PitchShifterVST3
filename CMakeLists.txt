cmake_minimum_required(VERSION 3.22)

project(BassPitchShifter VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(APPLE)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
    set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
endif()

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.4
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(BassPitchShifter
    COMPANY_NAME                "WolfBlack"
    PLUGIN_MANUFACTURER_CODE    Wblk
    PLUGIN_CODE                 Bpsh
    PRODUCT_NAME                "Bass Pitch Shifter"
    FORMATS                     VST3
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    VST3_CATEGORIES             Fx Pitch Shift)

target_sources(BassPitchShifter PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp)

target_compile_definitions(BassPitchShifter PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(BassPitchShifter
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Offline DSP test (no GUI, no host) - checks the shifter lands on the right pitch.
option(BPS_BUILD_TESTS "Build the offline DSP test" ON)
if(BPS_BUILD_TESTS)
    juce_add_console_app(DspTest PRODUCT_NAME "DspTest")
    target_sources(DspTest PRIVATE tests/DspTest.cpp)
    target_include_directories(DspTest PRIVATE Source)
    target_compile_definitions(DspTest PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0)
    target_link_libraries(DspTest PRIVATE juce::juce_dsp juce::juce_recommended_config_flags)
endif()
