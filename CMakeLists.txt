cmake_minimum_required(VERSION 3.22)

# Universal (Apple silicon + Intel) builds that run on macOS 10.13 and later.
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")

project(GittoFX VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release)
endif()

option(GITTO_BUILD_PREVIEW "Build the screenshot and offline-render tool" OFF)
option(GITTO_COPY_AFTER_BUILD "Install each plugin into the user plugin folders after building" OFF)

# JUCE: use a local checkout if JUCE_DIR is given, otherwise download the pinned release.
if(DEFINED JUCE_DIR)
    add_subdirectory(${JUCE_DIR} juce EXCLUDE_FROM_ALL)
else()
    include(FetchContent)
    FetchContent_Declare(juce
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG 8.0.9
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(juce)
endif()

if(APPLE)
    set(GITTO_FORMATS VST3 AU)
else()
    set(GITTO_FORMATS VST3)
endif()

set(GITTO_DEFINITIONS
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_MODAL_LOOPS_PERMITTED=1)

# gitto_add_plugin(<folder> <display name> <4-char code> <vst3 category>)
function(gitto_add_plugin folder name code category)
    string(TOLOWER ${folder} lower)
    set(target GittoFX${folder})
    juce_add_plugin(${target}
        VERSION ${PROJECT_VERSION}
        COMPANY_NAME "JG Productions"
        COMPANY_COPYRIGHT "JG Productions"
        BUNDLE_ID "com.jgproductions.gittofx.${lower}"
        PLUGIN_MANUFACTURER_CODE Jgpr
        PLUGIN_CODE ${code}
        FORMATS ${GITTO_FORMATS}
        PRODUCT_NAME "Gitto FX ${name}"
        DESCRIPTION "Gitto FX ${name}"
        IS_SYNTH FALSE
        NEEDS_MIDI_INPUT FALSE
        NEEDS_MIDI_OUTPUT FALSE
        IS_MIDI_EFFECT FALSE
        EDITOR_WANTS_KEYBOARD_FOCUS FALSE
        VST3_CATEGORIES Fx ${category}
        AU_MAIN_TYPE kAudioUnitType_Effect
        COPY_PLUGIN_AFTER_BUILD ${GITTO_COPY_AFTER_BUILD})
    target_sources(${target} PRIVATE plugins/${folder}/Plugin.cpp)
    target_compile_definitions(${target} PUBLIC ${GITTO_DEFINITIONS})
    target_link_libraries(${target}
        PRIVATE juce::juce_audio_utils
        PUBLIC juce::juce_recommended_config_flags)
endfunction()

gitto_add_plugin(EQ         "EQ"         Gfeq EQ)
gitto_add_plugin(Compressor "Compressor" Gfcm Dynamics)
gitto_add_plugin(Limiter    "Limiter"    Gflm Dynamics)
gitto_add_plugin(Reverb     "Reverb"     Gfrv Reverb)
gitto_add_plugin(Delay      "Delay"      Gfdl Delay)
gitto_add_plugin(Chorus     "Chorus"     Gfch Modulation)
gitto_add_plugin(Multiband    "Multiband"     Gfmb Dynamics)
gitto_add_plugin(Smooth       "Smooth"        Gfsm EQ)
gitto_add_plugin(OptoComp     "Opto Comp"     Gfop Dynamics)
gitto_add_plugin(FetComp      "FET Comp"      Gffe Dynamics)
gitto_add_plugin(Space        "Space"         Gfsp Reverb)
gitto_add_plugin(BusComp      "Bus Comp"      Gfbu Dynamics)
gitto_add_plugin(Tape         "Tape"          Gftp Distortion)
gitto_add_plugin(Plate        "Plate"         Gfpl Reverb)
gitto_add_plugin(Tune         "Tune"          Gftu "Pitch Shift")
gitto_add_plugin(Saturator    "Saturator"     Gfsa Distortion)
gitto_add_plugin(DeEsser      "De-Esser"      Gfde Dynamics)
gitto_add_plugin(Distortion   "Distortion"    Gfdi Distortion)
gitto_add_plugin(Shaper       "Shaper"        Gfsh Modulation)
gitto_add_plugin(ChannelStrip "Channel Strip" Gfcs Mixing)

if(GITTO_BUILD_PREVIEW)
    juce_add_console_app(GittoPreview PRODUCT_NAME "GittoPreview")
    target_sources(GittoPreview PRIVATE
        tests/Preview.cpp
        plugins/EQ/Plugin.cpp
        plugins/Compressor/Plugin.cpp
        plugins/Limiter/Plugin.cpp
        plugins/Reverb/Plugin.cpp
        plugins/Delay/Plugin.cpp
        plugins/Chorus/Plugin.cpp
        plugins/Multiband/Plugin.cpp
        plugins/Smooth/Plugin.cpp
        plugins/OptoComp/Plugin.cpp
        plugins/FetComp/Plugin.cpp
        plugins/Space/Plugin.cpp
        plugins/BusComp/Plugin.cpp
        plugins/Tape/Plugin.cpp
        plugins/Plate/Plugin.cpp
        plugins/Tune/Plugin.cpp
        plugins/Saturator/Plugin.cpp
        plugins/DeEsser/Plugin.cpp
        plugins/Distortion/Plugin.cpp
        plugins/Shaper/Plugin.cpp
        plugins/ChannelStrip/Plugin.cpp)
    target_compile_definitions(GittoPreview PRIVATE ${GITTO_DEFINITIONS} GITTO_PREVIEW=1)
    target_link_libraries(GittoPreview
        PRIVATE juce::juce_audio_utils
        PUBLIC juce::juce_recommended_config_flags)
endif()
