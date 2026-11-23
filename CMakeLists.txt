cmake_minimum_required(VERSION 3.22)

project(CUSTOM_AMP)

# The version comes from the newest git tag (like v0.1.0). A release build shows the tag,
# other builds show how far past it they are, like v0.1.0-2-gabc1234.
set(CUSTOM_AMP_VERSION_TEXT "dev")
set(CUSTOM_AMP_VERSION_NUMBER "0.0.0")
find_package(Git QUIET)

if(GIT_FOUND)
    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --match "v[0-9]*"
                    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                    OUTPUT_VARIABLE gitDescribe
                    OUTPUT_STRIP_TRAILING_WHITESPACE
                    ERROR_QUIET
                    RESULT_VARIABLE gitDescribeResult)

    if(gitDescribeResult EQUAL 0 AND gitDescribe MATCHES "^v([0-9]+[.][0-9]+[.][0-9]+)")
        set(CUSTOM_AMP_VERSION_TEXT "${gitDescribe}")
        set(CUSTOM_AMP_VERSION_NUMBER "${CMAKE_MATCH_1}")
    endif()
endif()

message(STATUS "Custom Amp version: ${CUSTOM_AMP_VERSION_TEXT}")

# Put the Visual C++ runtime inside the plugin, so it works on PCs without the Visual C++ Redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_CXX_STANDARD 20)

add_subdirectory(libs/JUCE)

# The amp engine: the C++ copy of the Python prototype in ampsim/.
set(ENGINE_SOURCES
    plugin/Source/dsp/Blocks.cpp
    plugin/Source/dsp/Circuit.cpp
    plugin/Source/dsp/Common.cpp
    plugin/Source/dsp/Convolver.cpp
    plugin/Source/dsp/Curves.cpp
    plugin/Source/dsp/Engine.cpp
    plugin/Source/dsp/Filters.cpp)

# Company name and manufacturer code are the same as in stream-bridge, so DAWs list both plugins
# under "Kursat". Never change PLUGIN_CODE: DAWs use it to find the plugin in saved projects.
juce_add_plugin(CustomAmp
    VERSION ${CUSTOM_AMP_VERSION_NUMBER}
    COMPANY_NAME "Kursat"
    PLUGIN_MANUFACTURER_CODE Kurs
    PLUGIN_CODE Camp
    FORMATS VST3 Standalone
    PRODUCT_NAME "Custom Amp"
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)

# The built-in presets and the help text for every setting, inside the plugin.
file(GLOB PRESET_FILES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/presets/*.json")
juce_add_binary_data(CustomAmpData SOURCES ${PRESET_FILES} shared/settings.json)

target_sources(CustomAmp
    PRIVATE
        ${ENGINE_SOURCES}
        plugin/Source/ChainView.cpp
        plugin/Source/Help.cpp
        plugin/Source/Inspector.cpp
        plugin/Source/PluginEditor.cpp
        plugin/Source/PluginProcessor.cpp)

target_compile_definitions(CustomAmp
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        "CUSTOM_AMP_VERSION=\"${CUSTOM_AMP_VERSION_TEXT}\"")

target_link_libraries(CustomAmp
    PRIVATE
        CustomAmpData
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Test program: drives the plugin like a DAW does (presets, file watching, quality, saved state, editor).
juce_add_console_app(PluginTest
    VERSION ${CUSTOM_AMP_VERSION_NUMBER}
    PRODUCT_NAME "Plugin Test")

target_sources(PluginTest
    PRIVATE
        ${ENGINE_SOURCES}
        plugin/Source/ChainView.cpp
        plugin/Source/Help.cpp
        plugin/Source/Inspector.cpp
        plugin/Source/PluginEditor.cpp
        plugin/Source/PluginProcessor.cpp
        plugin/tests/PluginTest.cpp)

target_compile_definitions(PluginTest
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_MODAL_LOOPS_PERMITTED=1
        "JucePlugin_Name=\"Custom Amp\""
        "CUSTOM_AMP_VERSION=\"${CUSTOM_AMP_VERSION_TEXT}\"")

target_link_libraries(PluginTest
    PRIVATE
        CustomAmpData
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)

# Test program: runs the engine on files, to compare it with the Python prototype (tools/cpp_check.py).
juce_add_console_app(EngineTest
    VERSION ${CUSTOM_AMP_VERSION_NUMBER}
    PRODUCT_NAME "Engine Test")

target_sources(EngineTest
    PRIVATE
        ${ENGINE_SOURCES}
        plugin/tests/EngineTest.cpp)

target_compile_definitions(EngineTest
    PRIVATE
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0)

target_link_libraries(EngineTest
    PRIVATE
        juce::juce_audio_formats
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
