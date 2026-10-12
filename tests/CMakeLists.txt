CPMAddPackage(
    NAME Catch2
    GITHUB_REPOSITORY catchorg/Catch2
    GIT_TAG v3.16.1
    OPTIONS "CATCH_INSTALL_DOCS OFF" "CATCH_INSTALL_EXTRAS OFF")
list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
include(Catch)

add_executable(lattice_tests
    test_main.cpp
    control/ComputerKeyboardTests.cpp
    core/AudioCallbackStatsTests.cpp
    core/LiveSetTests.cpp
    core/RealtimeCheckTests.cpp
    core/RtSharedTests.cpp
    core/StartupTimelineTests.cpp
    devices/DspTests.cpp
    devices/EffectTests.cpp
    devices/FrameworkTests.cpp
    devices/MidiEffectTests.cpp
    devices/RackTests.cpp
    devices/SamplerTests.cpp
    devices/SynthTests.cpp
    devices/TuningTestSynthTests.cpp
    engine/AudioEngineTests.cpp
    engine/SessionEngineTests.cpp
    engine/SnapshotStressTests.cpp
    engine/TestToneTests.cpp
    platform/PathsTests.cpp
    resources/ResourceTests.cpp
    tuning/ColundiParserTests.cpp
    tuning/ScalaAndRegistryTests.cpp
    tuning/TrackTuningTests.cpp)
target_include_directories(lattice_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lattice_tests PRIVATE lattice_core Catch2::Catch2)
if(NOT LATTICE_TSAN) # ThreadSanitizer's runtime defines operator new/delete itself
    target_link_libraries(lattice_tests PRIVATE lattice_alloc_hooks)
endif()
target_compile_definitions(lattice_tests PRIVATE LATTICE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
lattice_set_warnings(lattice_tests)

# Tags become ctest labels. Heavy tags ([slow] [bench] [stress] [hardware] [timing]) are left out
# of the default runs with: ctest -LE "slow|bench|stress|hardware|timing"
catch_discover_tests(lattice_tests
    ADD_TAGS_AS_LABELS
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 120)

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
