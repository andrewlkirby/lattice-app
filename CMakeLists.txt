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
        JUCE_USE_MP3AUDIOFORMAT=1
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
# The Colundi table and the factory presets are compiled in, so the app works without the files on
# disk. The presets are generated (tests/support/FactoryPresets.h).
file(GLOB LATTICE_FACTORY_PRESETS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/resources/presets/*.lpreset)
juce_add_binary_data(lattice_binary_data
    HEADER_NAME LatticeBinaryData.h
    NAMESPACE LatticeResources
    SOURCES resources/tunings/colundi_scale.txt ${LATTICE_FACTORY_PRESETS})
# Plain data needs no link-time optimisation (and MSVC's LTCG has crashed generating it).
set_target_properties(lattice_binary_data PROPERTIES FOLDER "Dependencies" POSITION_INDEPENDENT_CODE ON
                                                     INTERPROCEDURAL_OPTIMIZATION_RELEASE OFF)

# --- lattice_core: all non-UI Lattice code ------------------------------------------------------
add_library(lattice_core STATIC
    src/control/ComputerKeyboardInput.cpp
    src/core/AudioCallbackStats.cpp
    src/core/JobPool.cpp
    src/core/LatencyStats.cpp
    src/core/RealtimeCheck.cpp
    src/core/StartupTimeline.cpp
    src/core/model/LiveSet.cpp
    src/core/model/MidiImport.cpp
    src/devices/BuiltinDevices.cpp
    src/devices/ChainEditing.cpp
    src/devices/dsp/Biquad.cpp
    src/devices/dsp/FirHalfBand.cpp
    src/devices/dsp/HalfBand.cpp
    src/devices/dsp/OscillatorSimd.cpp
    src/devices/dsp/OscillatorSimdAvx2.cpp
    src/devices/dsp/ReverbTail.cpp
    src/devices/dsp/ReverbTailAvx2.cpp
    src/devices/dsp/Simd.cpp
    src/devices/dsp/SincBlock.cpp
    src/devices/dsp/SincBlockAvx2.cpp
    src/devices/dsp/SincResampler.cpp
    src/devices/dsp/VoiceFilterSimd.cpp
    src/devices/dsp/VoiceFilterSimdAvx2.cpp
    src/devices/dsp/Wavetable.cpp
    src/devices/effects/AutoFilter.cpp
    src/devices/effects/Chorus.cpp
    src/devices/effects/Compressor.cpp
    src/devices/effects/Delay.cpp
    src/devices/effects/EqEight.cpp
    src/devices/effects/Limiter.cpp
    src/devices/effects/Reverb.cpp
    src/devices/effects/Saturator.cpp
    src/devices/effects/Utility.cpp
    src/devices/framework/DeviceChain.cpp
    src/devices/framework/DeviceParameter.cpp
    src/devices/framework/DeviceRegistry.cpp
    src/devices/framework/ParameterPager.cpp
    src/devices/framework/ParameterSpec.cpp
    src/devices/framework/StudioDevice.cpp
    src/devices/instruments/Sampler.cpp
    src/devices/instruments/Synth.cpp
    src/devices/midi/Arpeggiator.cpp
    src/devices/midi/Chord.cpp
    src/devices/racks/Rack.cpp
    src/devices/presets/Presets.cpp
    src/devices/racks/RackEditing.cpp
    src/devices/racks/RackLayout.cpp
    src/devices/samples/SamplePool.cpp
    src/devices/test/TestSynthDevice.cpp
    src/devices/test/TuningTestSynth.cpp
    src/engine/AudioEngine.cpp
    src/engine/Metronome.cpp
    src/engine/MidiInputQueue.cpp
    src/engine/SessionController.cpp
    src/engine/SnapshotBuilder.cpp
    src/engine/TestToneSource.cpp
    src/engine/TrackProcessor.cpp
    src/engine/WorkerPool.cpp
    src/engine/Transport.cpp
    src/engine/session/ClipData.cpp
    src/engine/session/ClipPlayer.cpp
    src/engine/session/SessionProcessor.cpp
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
    target_link_libraries(lattice_core PRIVATE avrt) # MMCSS for the audio worker threads
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

# AVX2 + FMA kernels: every *Avx2.cpp in lattice_core, chosen at run time on CPUs that have them
# (devices/dsp/Simd.h, docs/perf/avx2-plan.md). Only these files get the flags; they include
# nothing but intrinsics and plain interfaces (see SincBlock.h), and they stay out of link-time
# optimisation, so their code is never inlined into code for any CPU.
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|x86_64|amd64)$")
    get_target_property(LATTICE_AVX2_SOURCES lattice_core SOURCES)
    list(FILTER LATTICE_AVX2_SOURCES INCLUDE REGEX "Avx2[.]cpp$")
    if(MSVC)
        set_source_files_properties(${LATTICE_AVX2_SOURCES} PROPERTIES COMPILE_OPTIONS "/arch:AVX2;/GL-")
    else()
        set_source_files_properties(${LATTICE_AVX2_SOURCES} PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma;-fno-lto")
    endif()
endif()

# --- Debug UI (M2/M3, replaced in M4): a library, so the tests can drive the editors headless ------
add_library(lattice_debug_ui STATIC
    src/app/debug/ClipPropertiesPanel.cpp
    src/app/debug/DetailView.cpp
    src/app/debug/DevicePanel.cpp
    src/app/debug/DeviceUi.cpp
    src/app/debug/MixerStrip.cpp
    src/app/debug/SessionGrid.cpp
    src/app/debug/SessionView.cpp
    src/app/debug/TransportBar.cpp)
target_include_directories(lattice_debug_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lattice_debug_ui PUBLIC lattice_core)
lattice_set_warnings(lattice_debug_ui)

# --- UI (M4, docs/05-m4-ui.md): a library, so the tests can drive the views headless ---------------
add_library(lattice_ui STATIC
    src/ui/commands/CommandRegistry.cpp
    src/ui/commands/SessionCommands.cpp
    src/ui/feed/EngineStateFeed.cpp
    src/ui/shell/ControlBar.cpp
    src/ui/shell/InfoView.cpp
    src/ui/shell/MainShell.cpp
    src/ui/shell/StatusBar.cpp
    src/ui/theme/LatticeLookAndFeel.cpp
    src/ui/theme/Theme.cpp
    src/ui/widgets/BoundControl.cpp
    src/ui/widgets/Fader.cpp
    src/ui/widgets/Knob.cpp
    src/ui/widgets/Meter.cpp
    src/ui/widgets/NumberBox.cpp
    src/ui/widgets/Toggle.cpp
    src/ui/widgets/ValueBinding.cpp)
target_include_directories(lattice_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(lattice_ui PUBLIC lattice_core)
lattice_set_warnings(lattice_ui)

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
    target_link_libraries(Lattice PRIVATE lattice_core lattice_debug_ui lattice_ui)
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
