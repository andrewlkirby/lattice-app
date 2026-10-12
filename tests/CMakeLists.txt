CPMAddPackage(
    NAME Catch2
    GITHUB_REPOSITORY catchorg/Catch2
    GIT_TAG v3.16.1
    OPTIONS "CATCH_INSTALL_DOCS OFF" "CATCH_INSTALL_EXTRAS OFF")
list(APPEND CMAKE_MODULE_PATH "${Catch2_SOURCE_DIR}/extras")
include(Catch)

add_executable(lattice_tests
    test_main.cpp
    core/AudioCallbackStatsTests.cpp
    core/RealtimeCheckTests.cpp
    core/StartupTimelineTests.cpp
    engine/AudioEngineTests.cpp
    engine/TestToneTests.cpp
    platform/PathsTests.cpp
    resources/ResourceTests.cpp)
target_include_directories(lattice_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lattice_tests PRIVATE lattice_core lattice_alloc_hooks Catch2::Catch2)
target_compile_definitions(lattice_tests PRIVATE LATTICE_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
lattice_set_warnings(lattice_tests)

# Tags become ctest labels. Heavy tags ([slow] [bench] [stress] [hardware] [timing]) are left out
# of the default runs with: ctest -LE "slow|bench|stress|hardware|timing"
catch_discover_tests(lattice_tests
    ADD_TAGS_AS_LABELS
    DISCOVERY_MODE PRE_TEST
    PROPERTIES TIMEOUT 120)
