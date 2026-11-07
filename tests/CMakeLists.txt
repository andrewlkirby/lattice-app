CPMAddPackage(
    NAME Catch2
    GITHUB_REPOSITORY catchorg/Catch2
    GIT_TAG v3.16.1
    OPTIONS "CATCH_INSTALL_DOCS OFF" "CATCH_INSTALL_EXTRAS OFF")
list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
include(Catch)

add_executable(lattice_tests
    test_main.cpp
    app/HandsOnTests.cpp
    control/ComputerKeyboardTests.cpp
    control/DeviceMixTests.cpp
    control/StepSequencerTests.cpp
    control/ClipBrowseTests.cpp
    control/FastPathTests.cpp
    control/MultiSurfaceTests.cpp
    control/GenericMidiTests.cpp
    control/SurfaceManagerTests.cpp
    control/SurfaceLearnerTests.cpp
    control/NoteModeTests.cpp
    control/PushTests.cpp
    control/SessionModeTests.cpp
    control/VirtualPushTests.cpp
    control/WalkthroughTests.cpp
    core/AudioCallbackStatsTests.cpp
    core/GroupTrackTests.cpp
    core/JobPoolTests.cpp
    core/LiveSetTests.cpp
    core/RealtimeCheckTests.cpp
    core/RtSharedTests.cpp
    core/StartupTimelineTests.cpp
    devices/ChainEditingTests.cpp
    devices/DspTests.cpp
    devices/EffectTests.cpp
    devices/FrameworkTests.cpp
    devices/MidiEffectTests.cpp
    devices/PresetTests.cpp
    devices/RackTests.cpp
    devices/SamplerTests.cpp
    devices/SimdTests.cpp
    devices/SynthTests.cpp
    devices/TuningTestSynthTests.cpp
    engine/AudioEngineTests.cpp
    engine/GroupTrackEngineTests.cpp
    engine/LiveInputTests.cpp
    engine/MixerRoutingTests.cpp
    engine/ParallelProcessingTests.cpp
    engine/SessionEngineTests.cpp
    engine/SnapshotStressTests.cpp
    engine/TestToneTests.cpp
    engine/TrackTuningSettingsTests.cpp
    hosting/PluginScanTests.cpp
    hosting/PluginWalkthroughTests.cpp
    library/LibraryTests.cpp
    platform/PathsTests.cpp
    resources/ResourceTests.cpp
    tuning/ColundiParserTests.cpp
    tuning/ScalaAndRegistryTests.cpp
    tuning/TuningEditsTests.cpp
    tuning/TrackTuningTests.cpp
    ui/BrowserTests.cpp
    ui/ClipViewTests.cpp
    ui/DeviceViewTests.cpp
    ui/CommandTests.cpp
    ui/FeedTests.cpp
    ui/SessionViewTests.cpp
    ui/ShellTests.cpp
    ui/FilesTests.cpp
    ui/EngineEditTests.cpp
    ui/FindTests.cpp
    ui/UndoFuzzTests.cpp
    ui/UndoNamesTests.cpp
    ui/MappingTests.cpp
    ui/PreferencesTests.cpp
    ui/ThemeTests.cpp
    ui/TuningViewTests.cpp
    ui/WidgetTests.cpp
    ui/AccessibilityTests.cpp
    ui/SnapshotTests.cpp)
target_include_directories(lattice_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lattice_tests PRIVATE lattice_core lattice_ui lattice_app Catch2::Catch2)
if(NOT LATTICE_TSAN) # ThreadSanitizer's runtime defines operator new/delete itself
    target_link_libraries(lattice_tests PRIVATE lattice_alloc_hooks)
endif()
target_compile_definitions(lattice_tests PRIVATE LATTICE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
lattice_set_warnings(lattice_tests)

# --- Test plugins (docs/07-m6-plugins-external.md, "Tests" 1) -------------------------------------
# Built into the build folder only: never into a system plugin folder.
set(LATTICE_TEST_PLUGINS "${CMAKE_CURRENT_BINARY_DIR}/test-plugins")
if(WIN32)
    set(_lattice_vst3_arch "x86_64-win")
    set(_lattice_vst3_suffix ".vst3")
elseif(APPLE)
    set(_lattice_vst3_arch "MacOS")
    set(_lattice_vst3_suffix "")
else()
    set(_lattice_vst3_arch "${CMAKE_SYSTEM_PROCESSOR}-linux")
    set(_lattice_vst3_suffix ".so")
endif()

# A plugin file made by hand: `kind` VST3 (in its bundle) or CLAP; `source` its code; the rest are
# its compile definitions.
function(lattice_test_plugin target kind name source)
    add_library(${target} MODULE ${source})
    target_compile_definitions(${target} PRIVATE ${ARGN})
    target_link_libraries(${target} PRIVATE lattice_clap)
    if(kind STREQUAL "VST3")
        set(bundle "${LATTICE_TEST_PLUGINS}/${target}/${name}.vst3")
        set(dir "${bundle}/Contents/${_lattice_vst3_arch}")
        set_target_properties(${target} PROPERTIES SUFFIX "${_lattice_vst3_suffix}")
    else()
        set(bundle "${LATTICE_TEST_PLUGINS}/${target}/${name}.clap")
        set(dir "${LATTICE_TEST_PLUGINS}/${target}")
        set_target_properties(${target} PROPERTIES SUFFIX ".clap")
    endif()
    set_target_properties(${target} PROPERTIES
        PREFIX ""
        OUTPUT_NAME "${name}"
        LIBRARY_OUTPUT_DIRECTORY "$<1:${dir}>"
        ARCHIVE_OUTPUT_DIRECTORY "$<1:${CMAKE_CURRENT_BINARY_DIR}/test-plugin-libs>"
        CXX_VISIBILITY_PRESET hidden
        FOLDER "Tests/Plugins")
    add_dependencies(lattice_tests ${target})
    string(TOUPPER "${target}" key)
    target_compile_definitions(lattice_tests PRIVATE "LATTICE_${key}=\"${bundle}\"")
endfunction()

lattice_test_plugin(test_clap CLAP "Lattice Test Clap" fixtures/plugins/TestClap.cpp)
lattice_test_plugin(crash_clap CLAP "Lattice Crash Clap" fixtures/plugins/BrokenPlugin.cpp LATTICE_BROKEN_CLAP LATTICE_BROKEN_CRASH)
lattice_test_plugin(hang_clap CLAP "Lattice Hang Clap" fixtures/plugins/BrokenPlugin.cpp LATTICE_BROKEN_CLAP LATTICE_BROKEN_HANG)
lattice_test_plugin(crash_vst3 VST3 "Lattice Crash VST3" fixtures/plugins/BrokenPlugin.cpp LATTICE_BROKEN_VST3 LATTICE_BROKEN_CRASH)
lattice_test_plugin(hang_vst3 VST3 "Lattice Hang VST3" fixtures/plugins/BrokenPlugin.cpp LATTICE_BROKEN_VST3 LATTICE_BROKEN_HANG)

# A real plugin, built with JUCE: a VST3 everywhere, and an LV2 on Linux.
set(_lattice_test_plugin_formats VST3)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    list(APPEND _lattice_test_plugin_formats LV2)
endif()
juce_add_plugin(LatticeTestPlugin
    PRODUCT_NAME "Lattice Test Plugin"
    COMPANY_NAME "Lattice Tests"
    BUNDLE_ID org.lattice.tests.plugin
    PLUGIN_MANUFACTURER_CODE Ltts
    PLUGIN_CODE Ltp1
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    LV2URI "urn:lattice:tests:plugin"
    FORMATS ${_lattice_test_plugin_formats})
target_sources(LatticeTestPlugin PRIVATE fixtures/plugins/TestPlugin.cpp)
target_compile_definitions(LatticeTestPlugin PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)
target_link_libraries(LatticeTestPlugin PRIVATE juce::juce_audio_utils PUBLIC juce::juce_recommended_config_flags)
set_target_properties(LatticeTestPlugin LatticeTestPlugin_VST3 PROPERTIES FOLDER "Tests/Plugins")
add_dependencies(lattice_tests LatticeTestPlugin_VST3)
# The bundle is two folders above the binary.
target_compile_definitions(lattice_tests PRIVATE "LATTICE_TEST_VST3_BINARY_DIR=\"$<TARGET_FILE_DIR:LatticeTestPlugin_VST3>\"")
if(TARGET LatticeTestPlugin_LV2)
    set_target_properties(LatticeTestPlugin_LV2 PROPERTIES FOLDER "Tests/Plugins")
    add_dependencies(lattice_tests LatticeTestPlugin_LV2)
    target_compile_definitions(lattice_tests PRIVATE "LATTICE_TEST_LV2=\"$<TARGET_FILE_DIR:LatticeTestPlugin_LV2>\"")
endif()

add_dependencies(lattice_tests LatticePluginScanner)
# Beside the tests too, where a shell's plugin library looks for it (PluginLibrary::scannerBesideApp).
add_custom_command(TARGET lattice_tests POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:LatticePluginScanner>" "$<TARGET_FILE_DIR:lattice_tests>"
    VERBATIM)
target_compile_definitions(lattice_tests PRIVATE "LATTICE_PLUGIN_SCANNER=\"$<TARGET_FILE:LatticePluginScanner>\"")

# Tags become ctest labels. Heavy tags ([slow] [bench] [stress] [hardware] [timing]) are left out
# of the default runs with: ctest -LE "slow|bench|stress|hardware|timing"
catch_discover_tests(lattice_tests
    ADD_TAGS_AS_LABELS
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 120)

# The device tests again on the SSE2 kernels (docs/perf/avx2-plan.md, "Testing"): on an AVX2
# machine the run above takes the AVX2 paths, this one the SSE2 ones.
add_test(NAME "Device tests on the SSE2 kernels"
         COMMAND lattice_tests "[devices]~[heavy]~[slow]~[bench]~[timing]~[stress]")
set_tests_properties("Device tests on the SSE2 kernels" PROPERTIES
    ENVIRONMENT "LATTICE_SIMD=sse2"
    LABELS "devices;simd"
    TIMEOUT 900)

# Engine benchmark: renders the performance reference set offline (docs/09-performance-charter.md).
# scripts/linux-test.sh --callgrind counts its instructions against docs/perf/baselines/.
add_executable(lattice_engine_bench bench/EngineBench.cpp)
target_include_directories(lattice_engine_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lattice_engine_bench PRIVATE lattice_core)
lattice_set_warnings(lattice_engine_bench)

# Device benchmarks (docs/04-m3-devices.md, "Tests" 11): each device's share of one core.
add_executable(lattice_bench bench/DeviceBench.cpp)
target_include_directories(lattice_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lattice_bench PRIVATE lattice_core)
lattice_set_warnings(lattice_bench)

# UI benchmarks (docs/05-m4-ui.md, "Tests" 10): frame times of the busiest views against the
# charter's 8 ms. scripts/linux-test.sh --callgrind counts its instructions too.
add_executable(lattice_ui_bench bench/UiBench.cpp)
target_include_directories(lattice_ui_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lattice_ui_bench PRIVATE lattice_core lattice_ui)
lattice_set_warnings(lattice_ui_bench)
