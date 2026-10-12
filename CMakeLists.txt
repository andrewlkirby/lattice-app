cmake_minimum_required(VERSION 3.25)

project(Lattice VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)

# --- Options -------------------------------------------------------------------------------------
set(LATTICE_RT_CHECKS "AUTO" CACHE STRING
    "Real-time checks in the app (allocation hooks + audio-thread asserts): AUTO = Debug only, ON, OFF")
set_property(CACHE LATTICE_RT_CHECKS PROPERTY STRINGS AUTO ON OFF)
# ASIO: the Steinberg ASIO SDK is dual-licensed (Steinberg ASIO License or GPLv3). Lattice uses it
# under GPLv3 (personal, non-closed-source use; decided 2026-10-11). The SDK is downloaded at
# configure time and never committed.
if(WIN32)
    set(_lattice_asio_default ON)
else()
    set(_lattice_asio_default OFF)
endif()
option(LATTICE_ENABLE_ASIO "Build with Steinberg ASIO support (Windows)" ${_lattice_asio_default})
set(LATTICE_ASIO_SDK_DIR "" CACHE PATH "Use this extracted ASIO SDK (the folder containing common/) instead of downloading it")
option(LATTICE_PROFILE "Enable Tracy profiler markers" OFF)
option(LATTICE_RTSAN "Build with Clang's real-time sanitizer (-fsanitize=realtime, Clang 20+)" OFF)
option(LATTICE_TSAN "Build with ThreadSanitizer (-fsanitize=thread) to find data races" OFF)
option(LATTICE_BUILD_APP "Build the Lattice GUI app" ON)
option(LATTICE_BUILD_TESTS "Build the unit tests" ON)

lattice_set_global_codegen()

find_program(LATTICE_CCACHE ccache)
if(LATTICE_CCACHE AND NOT MSVC)
    set(CMAKE_C_COMPILER_LAUNCHER "${LATTICE_CCACHE}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${LATTICE_CCACHE}")
endif()
add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/MP>)

if(LATTICE_RTSAN)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_VERSION VERSION_LESS 20)
        message(FATAL_ERROR "LATTICE_RTSAN needs Clang 20 or newer")
    endif()
    add_compile_options(-fsanitize=realtime)
    add_link_options(-fsanitize=realtime)
endif()

if(LATTICE_TSAN)
    if(MSVC)
        message(FATAL_ERROR "LATTICE_TSAN needs Clang or GCC")
    endif()
    add_compile_options(-fsanitize=thread)
    add_link_options(-fsanitize=thread)
endif()

# --- Dependencies (pinned) -----------------------------------------------------------------------
# One shared download cache for every build folder unless the environment provides one
# (the Docker scripts mount a named volume there).
if(NOT DEFINED CPM_SOURCE_CACHE AND NOT DEFINED ENV{CPM_SOURCE_CACHE})
    set(CPM_SOURCE_CACHE "${CMAKE_CURRENT_SOURCE_DIR}/.cpm-cache" CACHE PATH "CPM download cache")
endif()
include(CPM)

CPMAddPackage(
    NAME JUCE
    GITHUB_REPOSITORY juce-framework/JUCE
    GIT_TAG 8.0.15
    OPTIONS "JUCE_BUILD_EXTRAS OFF" "JUCE_BUILD_EXAMPLES OFF")

if(LATTICE_PROFILE)
    CPMAddPackage(
        NAME tracy
        GITHUB_REPOSITORY wolfpld/tracy
        GIT_TAG v0.14.1
        OPTIONS "TRACY_ENABLE ON" "TRACY_ON_DEMAND ON")
endif()

# --- lattice_juce: every JUCE module, compiled exactly once ---------------------------------------
# JUCE modules are INTERFACE libraries that add their sources to whatever links them. Linking them
# PRIVATE here and re-exporting only their include paths and definitions means the app and the
# tests share one compiled copy instead of each compiling JUCE again.
add_library(lattice_juce STATIC)
target_link_libraries(lattice_juce
    PRIVATE
        juce::juce_audio_basics
        juce::juce_audio_devices
        juce::juce_audio_formats
        juce::juce_audio_processors
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_events
        juce::juce_gui_extra)
# PRIVATE here; the INTERFACE re-export below hands the same definitions to everything that
# includes JUCE headers, so the module configuration always matches the compiled modules.
target_compile_definitions(lattice_juce
    PRIVATE
        JUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_JACK=$<PLATFORM_ID:Linux>
        JUCE_ALSA=$<PLATFORM_ID:Linux>
        JUCE_WASAPI=1
        JUCE_DIRECTSOUND=0
        JUCE_STRICT_REFCOUNTEDPOINTER=1
        JUCE_DISABLE_JUCE_VERSION_PRINTING=1
        JUCE_MODAL_LOOPS_PERMITTED=0
        JUCE_ASIO=$<BOOL:${LATTICE_ENABLE_ASIO}>)
if(MSVC)
    target_compile_options(lattice_juce PRIVATE /bigobj)
endif()
if(LATTICE_ENABLE_ASIO)
    if(NOT WIN32)
        message(FATAL_ERROR "LATTICE_ENABLE_ASIO is only supported on Windows")
    endif()
    set(_lattice_asio_dir "${LATTICE_ASIO_SDK_DIR}")
    if(NOT _lattice_asio_dir)
        CPMAddPackage(
            NAME asiosdk
            VERSION 2.3.4
            URL https://download.steinberg.net/sdk_downloads/ASIO-SDK_2.3.4_2025-10-15.zip
            URL_HASH SHA256=d5ebf0c20dd2c5f43771fd0c1418f4b361bf52434ee670097cfa6b3a335e2eca
            DOWNLOAD_ONLY YES)
        set(_lattice_asio_dir "${asiosdk_SOURCE_DIR}")
    endif()
    if(NOT EXISTS "${_lattice_asio_dir}/common/iasiodrv.h")
        message(FATAL_ERROR "ASIO SDK not found at ${_lattice_asio_dir} (expected common/iasiodrv.h)")
    endif()
    target_include_directories(lattice_juce SYSTEM PRIVATE "${_lattice_asio_dir}/common")
endif()
target_include_directories(lattice_juce SYSTEM
    INTERFACE $<TARGET_PROPERTY:lattice_juce,INCLUDE_DIRECTORIES>)
target_compile_definitions(lattice_juce
    INTERFACE $<TARGET_PROPERTY:lattice_juce,COMPILE_DEFINITIONS>)
set_target_properties(lattice_juce PROPERTIES FOLDER "Dependencies")

# --- Embedded resources ----------------------------------------------------------------------------
# The Colundi table is compiled in, so the app works without the file on disk.
juce_add_binary_data(lattice_binary_data
    HEADER_NAME LatticeBinaryData.h
    NAMESPACE LatticeResources
    SOURCES resources/tunings/colundi_scale.txt)
set_target_properties(lattice_binary_data PROPERTIES FOLDER "Dependencies" POSITION_INDEPENDENT_CODE ON)

# --- lattice_core: all non-UI Lattice code ------------------------------------------------------
add_library(lattice_core STATIC
    src/control/ComputerKeyboardInput.cpp
    src/core/AudioCallbackStats.cpp
    src/core/RealtimeCheck.cpp
    src/core/StartupTimeline.cpp
    src/devices/test/TuningTestSynth.cpp
    src/engine/AudioEngine.cpp
    src/engine/MidiInputQueue.cpp
    src/engine/TestToneSource.cpp
    src/platform/PathsCommon.cpp
    src/tuning/TrackTuning.cpp
    src/tuning/Tet12.cpp
    src/tuning/TuningRegistry.cpp
    src/tuning/TuningTable.cpp
    src/tuning/parsers/ColundiTextParser.cpp
    src/tuning/parsers/NumberParsing.cpp
    src/tuning/parsers/ScalaParser.cpp)
if(WIN32)
    target_sources(lattice_core PRIVATE src/platform/windows/PlatformWindows.cpp)
elseif(APPLE)
    target_sources(lattice_core PRIVATE src/platform/macos/PlatformMacOS.cpp)
else()
    target_sources(lattice_core PRIVATE src/platform/linux/PlatformLinux.cpp)
endif()
target_include_directories(lattice_core PUBLIC src)
target_link_libraries(lattice_core PUBLIC lattice_juce PRIVATE lattice_binary_data)
target_compile_definitions(lattice_core PUBLIC LATTICE_PROFILE=$<BOOL:${LATTICE_PROFILE}>)
if(LATTICE_PROFILE)
    target_link_libraries(lattice_core PUBLIC Tracy::TracyClient)
endif()
lattice_set_warnings(lattice_core)

# Replacement operator new/delete that count allocations and flag any made on the audio thread.
# Always linked into the tests; linked into the app according to LATTICE_RT_CHECKS.
add_library(lattice_alloc_hooks OBJECT src/core/AllocationHooks.cpp)
target_link_libraries(lattice_alloc_hooks PRIVATE lattice_core)
lattice_set_warnings(lattice_alloc_hooks)

# --- App -----------------------------------------------------------------------------------------
if(LATTICE_BUILD_APP)
    juce_add_gui_app(Lattice
        PRODUCT_NAME "Lattice"
        COMPANY_NAME "Lattice"
        BUNDLE_ID "org.lattice.app"
        VERSION ${PROJECT_VERSION})
    target_sources(Lattice PRIVATE
        src/app/Main.cpp
        src/app/MainWindow.cpp
        src/app/PlayPanel.cpp
        src/app/SoakTest.cpp)
    target_compile_definitions(Lattice PRIVATE
        JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:Lattice,JUCE_PRODUCT_NAME>"
        JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:Lattice,JUCE_VERSION>")
    target_link_libraries(Lattice PRIVATE lattice_core)
    if(LATTICE_RT_CHECKS STREQUAL "ON")
        target_link_libraries(Lattice PRIVATE lattice_alloc_hooks)
        target_compile_definitions(Lattice PRIVATE LATTICE_RT_CHECKS_ENABLED=1)
    elseif(LATTICE_RT_CHECKS STREQUAL "AUTO")
        target_sources(Lattice PRIVATE $<$<CONFIG:Debug>:$<TARGET_OBJECTS:lattice_alloc_hooks>>)
        target_compile_definitions(Lattice PRIVATE LATTICE_RT_CHECKS_ENABLED=$<CONFIG:Debug>)
    else()
        target_compile_definitions(Lattice PRIVATE LATTICE_RT_CHECKS_ENABLED=0)
    endif()
    lattice_set_warnings(Lattice)
    set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Lattice)
endif()

# --- Tests ---------------------------------------------------------------------------------------
if(LATTICE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
