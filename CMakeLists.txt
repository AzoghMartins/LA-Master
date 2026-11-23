cmake_minimum_required(VERSION 3.22)

# macOS: one universal binary for Apple silicon and Intel Macs. Windows: link the C/C++ runtime
# statically so the plugin does not need the Visual C++ redistributable. (Both must be set before project().)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64" CACHE STRING "macOS architectures")
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

project(LAMaster VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(MSVC)
    # M_PI and friends; sources are UTF-8
    add_compile_definitions(_USE_MATH_DEFINES)
    add_compile_options(/utf-8)
endif()

option(LAM_BUILD_TESTS "Build the DSP and plugin tests" ON)
option(LAM_COPY_AFTER_BUILD "Install the plugins into the user/system plugin folders after building" OFF)
set(LAM_JUCE_DIR "" CACHE PATH "Path to a local JUCE checkout (fetched from GitHub when empty)")

# ---------------------------------------------------------------------------------------------
# DSP engine (no JUCE dependency, unit tested separately)
add_library(lam_engine STATIC
    Source/dsp/Loudness.cpp
    Source/dsp/Modules.cpp
    Source/dsp/MasterEngine.cpp
    Source/dsp/Assistant.cpp)
target_include_directories(lam_engine PUBLIC Source)
target_compile_options(lam_engine PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall -Wextra -O3>)

if(LAM_BUILD_TESTS)
    enable_testing()
    add_executable(lam_tests tests/EngineTests.cpp)
    target_link_libraries(lam_tests PRIVATE lam_engine)
    target_compile_options(lam_tests PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-O2>)
    add_test(NAME engine COMMAND lam_tests)
endif()

if(LAM_ENGINE_ONLY)
    return()
endif()

# ---------------------------------------------------------------------------------------------
# JUCE
if(LAM_JUCE_DIR)
    add_subdirectory(${LAM_JUCE_DIR} ${CMAKE_BINARY_DIR}/JUCE EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.4
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(JUCE)
endif()

set(LAM_PLUGIN_SOURCES
    Source/Parameters.cpp
    Source/PluginProcessor.cpp
    Source/gui/Theme.cpp
    Source/gui/Widgets.cpp
    Source/gui/Displays.cpp
    Source/gui/PluginEditor.cpp)

set(LAM_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_MP3AUDIOFORMAT=1)

# VST3 everywhere; LV2 + Standalone on Linux; AU on macOS
if(WIN32)
    set(LAM_FORMATS VST3)
elseif(APPLE)
    set(LAM_FORMATS VST3 AU)
else()
    set(LAM_FORMATS VST3 LV2 Standalone)
endif()

# ---------------------------------------------------------------------------------------------
# Plugin
juce_add_plugin(LAMaster
    PRODUCT_NAME "LA-Master"
    COMPANY_NAME "LA Audio"
    BUNDLE_ID "com.la-audio.lamaster"
    PLUGIN_MANUFACTURER_CODE LaAu
    PLUGIN_CODE LaMs
    FORMATS ${LAM_FORMATS}
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES Fx Mastering Dynamics EQ
    LV2URI "https://github.com/AzoghMartins/LA-Master"
    COPY_PLUGIN_AFTER_BUILD ${LAM_COPY_AFTER_BUILD})

target_sources(LAMaster PRIVATE ${LAM_PLUGIN_SOURCES})
target_compile_definitions(LAMaster PUBLIC ${LAM_DEFINITIONS} JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(LAMaster
    PRIVATE
        lam_engine
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# ---------------------------------------------------------------------------------------------
if(LAM_BUILD_TESTS)
    # Drives the real processor + editor like a host (needs an X display, e.g. xvfb-run).
    juce_add_console_app(lam_integration PRODUCT_NAME "LA-Master Integration Test")
    target_sources(lam_integration PRIVATE tests/PluginIntegrationTest.cpp ${LAM_PLUGIN_SOURCES})
    target_compile_definitions(lam_integration PRIVATE JucePlugin_Name="LA-Master" ${LAM_DEFINITIONS})
    target_link_libraries(lam_integration PRIVATE
        lam_engine
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_recommended_config_flags)
    add_test(NAME integration COMMAND lam_integration)
endif()
