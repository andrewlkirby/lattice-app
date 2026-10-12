# Sub-plan M0: Project Skeleton

Parent: `00-preliminary-plan.md`, milestone M0.

## Goal
Get to a buildable, testable, runnable JUCE app as early as possible. Every later milestone adds to it. At the end of M0:
- the app opens a window
- you can choose an audio device
- you can play a test tone
- unit tests run in CI on Windows, Linux, and macOS, and **locally on Linux through Docker**
- the real-time-safety rules are enforced from day one
- platform-specific code has a home (`src/platform/`) from the start

## Prerequisites
- **Windows:** Visual Studio 2022 (Desktop C++ workload), CMake 3.25 or newer, Ninja (optional), Git.
- **Linux testing:** Docker Desktop (with the WSL2 back end) on the Windows machine. No Linux PC is needed for now.
- **Linux, natively (later, optional):** Ubuntu 24.04, GCC 13 or Clang 18, and the packages listed in `docker/linux.Dockerfile`.
- **macOS (later):** Xcode 15 or newer.
- **Licensing, decide before release (not a blocker for development):**
  - JUCE 8 is dual-licensed: AGPLv3 or a commercial license. A closed-source release needs a JUCE license.
  - ASIO support needs Steinberg's ASIO SDK. Check its current license terms. WASAPI works without it, so ASIO is an optional build flag.
  - **Decided 2026-10-11:** Lattice is for personal use and won't be released as closed source. JUCE is used under AGPLv3. The ASIO SDK (2.3.4, dual-licensed: Steinberg ASIO License or GPLv3) is used under GPLv3. `LATTICE_ENABLE_ASIO` is ON by default on Windows. CMake downloads the SDK from Steinberg with a pinned SHA-256 hash, and it's never committed. Revisit both licenses if a release is ever planned.
- **Git:** the repo already exists at `lattice-app/` (set up by the user).

## Repository layout created in M0
```
lattice-app/
  CMakeLists.txt
  cmake/CPM.cmake               # dependency fetcher
  cmake/CompilerWarnings.cmake
  .clang-format  .editorconfig  .gitignore
  .gitattributes                # `* text=auto eol=lf`, so scripts work in the Linux container (Windows line endings break bash); `*.ps1`/`*.bat` stay CRLF
  src/app/Main.cpp              # JUCEApplication
  src/app/MainWindow.{h,cpp}
  src/engine/AudioEngine.{h,cpp}
  src/engine/TestToneSource.{h,cpp}
  src/core/RealtimeCheck.{h,cpp}
  src/platform/Platform.h       # shared interfaces (M0: user data/config folders only)
  src/platform/windows/  src/platform/linux/  src/platform/macos/
  tests/CMakeLists.txt
  tests/test_main.cpp
  tests/engine/TestToneTests.cpp
  resources/tunings/colundi_scale.txt   # copied from docs/colundi_scale.txt (the docs copy stays as the reference)
  CMakePresets.json             # windows-debug/release, linux-debug/release, macos-debug/release
  docker/linux.Dockerfile
  scripts/linux-test.sh  scripts/linux-test.ps1
  .github/workflows/ci.yml
  docs/
```
Empty `src/{tuning,devices,hosting,control,ui}` folders appear as each milestone starts. M0 does not create them.

## Build system
- **Dependencies through CPM.cmake, with pinned versions:**
  - JUCE 8.0.x
  - Catch2 v3.x
- **Targets:**
  - `lattice_core`: a static library that holds all non-UI code (engine, later tuning and devices). It links the JUCE modules `juce_audio_basics`, `juce_audio_devices`, `juce_audio_formats`, `juce_audio_processors`, `juce_dsp`, and `juce_events`. Tests link against it, so logic can be tested without a GUI.
  - `Lattice`: built with `juce_add_gui_app`. Links `lattice_core` and `juce_gui_extra`.
  - `lattice_tests`: a Catch2 console runner. Links `lattice_core`.
- **Linux settings in JUCE:** `JUCE_WEB_BROWSER=0` and `JUCE_USE_CURL=0`, so the build doesn't need WebKit or curl. ALSA and JACK are enabled (`JUCE_JACK=1`).
- **Options:**
  - `LATTICE_ENABLE_ASIO`: OFF until the ASIO SDK license is confirmed during M0. After that, ON for Windows builds made on the user's machine, so ASIO4ALL, FlexASIO, and hardware ASIO drivers work. CI keeps it off unless the license allows the SDK to be fetched automatically.
  - `LATTICE_RT_CHECKS` (ON in Debug)
- **Compiler settings:**
  - C++20 everywhere.
  - High warning levels: `/W4` on MSVC, `-Wall -Wextra` on GCC and Clang.
  - Both GCC and Clang build on Linux. CI uses Clang, and Docker's default is GCC, so both stay working.
- **Performance settings** (from `09-performance-charter.md` §5):
  - Release builds use `-O2`/`/O2` plus LTO, and the CPU baseline is x86-64-v2.
  - There's no global `-ffast-math`.
  - A **`relwithdebinfo-rtchecks`** preset is the everyday development build.
  - A **`linux-rtsan`** preset (Clang, `-fsanitize=realtime`).
  - A `LATTICE_PROFILE` option that turns on Tracy.
  - Warnings are treated as errors in CI only.

## Code
1. **`AudioEngine`** owns a `juce::AudioDeviceManager` and implements `juce::AudioIODeviceCallback`. For now it renders one `TestToneSource`. It is the seed of the real engine in M2:
   - one callback entry point
   - pre-allocated buffers in `audioDeviceAboutToStart`
2. **`TestToneSource`** is a sine oscillator.
   - Frequency and gain are `std::atomic<float>`.
   - The phase accumulator is a double.
   - The gain is smoothed with `juce::SmoothedValue` to avoid clicks.
3. **`MainWindow`** contains:
   - a `juce::AudioDeviceSelectorComponent`
   - a tone on/off toggle
   - a frequency slider
   - a CPU-load readout from `AudioDeviceManager::getCpuUsage`
   - saving the audio device setup to `juce::PropertiesFile` and restoring it on launch
4. **`RealtimeCheck`** (Debug only) enforces the real-time rules:
   - A `thread_local bool inAudioThread` flag, set by an RAII `ScopedAudioThread` guard at the top of the audio callback.
   - Global `operator new`/`delete` overrides that `jassertfalse` when the flag is set.
   - A `RT_ASSERT_NOT_AUDIO_THREAD()` macro to put in lock-taking code.
   - This catches allocation on the audio thread from the first day, as the risks section of the main plan requires.
   - It works the same on every platform: standard C++ only, with nothing OS-specific.
5. **Performance measurement** (charter §2, §7):
   - an `AudioCallbackStats` lock-free histogram (p50/p99/p99.9/max as a share of the buffer period) and a dropout counter, shown in the MainWindow's CPU readout
   - a `StartupTimeline` that logs each startup stage to `<userdata>/logs/startup.log`
   - Tracy markers in the callback and at startup (they compile away unless `LATTICE_PROFILE` is on)
   - the audio callback marked `[[clang::nonblocking]]`
   - the window shown **before** the audio device opens (the device opens on a background thread)
   - `scripts/perf-check.ps1` (a skeleton). `docs/perf/reference-machine.md` already exists; M0 fills in the remaining fields (OS build, storage, audio interface) and the LatencyMon result
6. **`platform::Paths`**, the first platform interface. It gives the user data folder and the settings folder for each OS (`<userdata>`, as defined in the preliminary plan). `PropertiesFile` uses it to decide where to save settings.

## Tests (Catch2)
- **Tag convention (from now on):** `[slow]`, `[bench]`, `[stress]`, `[hardware]`, `[timing]`. Tests with these tags are left out of the default runs (Docker and every-commit CI) and run nightly or on demand.
- `TestToneSource` renders 1 s at 48 kHz. Zero-crossing or Goertzel estimation finds 440 Hz ± 0.1 Hz, and the peak level matches the requested gain.
- With the gain ramp enabled, no sample-to-sample jump exceeds a set threshold (no clicks).
- The RealtimeCheck guard trips when an allocation happens inside the guard (a death-style test, or a test of the flag itself).
- `platform::Paths` gives the expected folders on each OS, including following `XDG_DATA_HOME` and `XDG_CONFIG_HOME` on Linux.
- **Performance gates from day one:**
  - an allocation-count test: rendering 10,000 test-tone blocks allocates 0 bytes
  - the RTSan build runs the test suite in Docker and CI
  - `clang-tidy` `performance-*` checks are clean
  - the callback histogram is tested with synthetic timings
  - the startup timeline records its stages in order

## Linux through Docker
- **`docker/linux.Dockerfile`:**
  - **Base:** Ubuntu 24.04.
  - **Tools:** `build-essential`, **Clang 20 or newer** (from apt.llvm.org; RTSan needs it), `clang-tidy`, `valgrind` (instruction counts), `cmake`, `ninja-build`, `ccache`, `git`, `pkg-config`.
  - **JUCE's Linux dependencies:** `libasound2-dev`, `libjack-jackd2-dev`, `libfreetype-dev`, `libfontconfig1-dev`, `libx11-dev`, `libxrandr-dev`, `libxinerama-dev`, `libxcursor-dev`, `libxext-dev`, `libxcomposite-dev`, `libgl1-mesa-dev`.
  - **For tests:** `xvfb`, `xauth`, and the `locales` package with `de_DE.UTF-8` generated (M1's locale test needs it).
  - The exact package list is fixed and checked when M0 is built.
- **`scripts/linux-test.ps1`** (run from Windows) and **`scripts/linux-test.sh`** (run from Linux or WSL):
  1. Build the image (cached after the first time).
  2. Run a container with the repo mounted, a named **ccache volume**, and a named CPM source-cache volume.
  3. Configure with the `linux-release` preset (or `linux-debug` with `--debug`), build with at most 4 parallel jobs, and run `ctest` without the heavy tags.
  4. UI tests run through `xvfb-run`.
  5. Options: `--all-tests` (include the heavy tags), `--clang`, and `--shell` (open a shell in the container to debug).
- **Compute budget:** the first image build plus the JUCE build takes a few minutes. After that, with ccache, a typical run should take **under 5 minutes** and use at most 4 cores and 4 GB of RAM. If later milestones push past this, the tests get re-tagged.
- **What Docker can't do:** it has no audio device, MIDI ports, USB, or real-time scheduling. The app itself isn't run in Docker. Running the app on Linux waits until a Linux machine (or a VM with audio pass-through) is available.

## CI (GitHub Actions)
- **Matrix:**
  - `windows-latest` (MSVC)
  - `ubuntu-24.04`, built **inside the same Docker image**, so CI and local runs match
  - `macos-latest` (build plus fast tests; tier 3)
- Steps:
  1. checkout
  2. cache CPM sources
  3. CMake configure (Release + Debug)
  4. build
  5. `ctest --output-on-failure`
- No ASIO in CI.
- **A nightly job** runs the heavy tags (`[slow]`, `[stress]`) on Windows and Linux. `[bench]` is not run on shared runners for pass/fail (see M3).

## Definition of done
- [x] A fresh clone runs `cmake --preset windows-debug && cmake --build --preset windows-debug` and builds with no manual steps.
- [x] The app launches, shows audio devices, and plays and stops a clean test tone. The device choice is saved between launches. (Checked by the user, 2026-10-11.)
- [ ] `ctest` passes locally on Windows, through `scripts/linux-test.ps1` in Docker, and in CI on Windows, Ubuntu, and macOS.
- [x] After the first build, a cached Docker test run finishes in under 5 minutes. (2026-10-11: cold GCC build 4.5 min; cached runs 50–56 s.)
- [x] **Performance gate (charter §3):** RTSan and the allocation-count tests pass. On the reference machine, the window appears in ≤ 0.5 s and the app is ready to use in ≤ 1.5 s, and the test tone runs at a 64-sample buffer with 0 dropouts over 10 minutes. The reference machine's details are recorded.
  - 2026-10-11: RTSan and the allocation-count tests pass. Startup: 0.36 s / 0.53 s. **Soak: 0 dropouts over 10 minutes at 3 ms (144 samples at 48 kHz), which is the smallest buffer the reference output (Schiit Magni Unity) allows through WASAPI.** **64 samples: 0 dropouts over 10 minutes through ASIO4ALL on the Magni** (it lowers the buffer but not the reported latency; see the comparison). The soak is now automated: `scripts\perf-check.ps1 -Soak` (Lattice `--soak-seconds/--buffer-size/--sample-rate/--device-type`; writes `<userdata>/logs/soak.log`). LatencyMon crashes on this Windows build (see `perf/reference-machine.md`).
- [x] A deliberate `new` inside the audio callback (temporary) triggers the Debug RT check.
- [x] On the reference laptop, **WASAPI exclusive and ASIO4ALL are compared** (smallest buffer with 0 dropouts over 10 minutes, and the reported latency). The chosen default is recorded in `docs/perf/reference-machine.md`. (2026-10-11: both stable over 10 minutes. ASIO4ALL at 64 samples / 7.62 ms reported, WASAPI exclusive at 144 / 6.00 ms. **WASAPI exclusive stays the default.**)
- [x] `resources/tunings/colundi_scale.txt` exists (copied from `docs/colundi_scale.txt`), ready for M1.

## As built (2026-10-11)
Where the code differs from the plan above, and why:
- **One compiled copy of JUCE.** `lattice_juce` is a static library that compiles every JUCE module once (including the GUI modules) and passes the include paths and module settings on to `lattice_core`, the app, and the tests. Linking the JUCE modules straight into `lattice_core` would make the app and the tests each compile JUCE again. JUCE's IDE source groups are off: with them on, a build without the app (the RTSan preset) compiled module files on their own and failed.
- **`JUCE_JACK` and `JUCE_ALSA` are set on Linux only.** JUCE builds JACK support on Windows too if asked, and fails without the JACK headers.
- **`LATTICE_RT_CHECKS` takes `AUTO` (Debug only), `ON`, or `OFF`.** The allocation hooks (`src/core/AllocationHooks.cpp`, replacement `operator new`/`delete`) are a separate object library. The tests always link it, so the allocation-count tests also run in Release, in Docker, and under RTSan. The app links it according to the option.
- **`LATTICE_NONBLOCKING`** expands to `[[clang::nonblocking]]` on Clang. It's checked to be live: RTSan aborts on a `malloc` inside such a function.
- **The audio device opens on a background thread, started before the window is built** (`AudioEngine::openDeviceAsync`). Device types are still created on the message thread, because WASAPI's device-change detector is a window. The open thread joins COM (`platform::ScopedAudioDeviceThread`). ASIO is opened on the message thread (`platform::audioDeviceTypeNeedsMessageThread`). This was needed for the startup gate: WASAPI exclusive mode takes about 1.2 s to open on the reference laptop (see `perf/reference-machine.md`). Until the open finishes, nothing else touches the `AudioDeviceManager`; the device selector is created only afterwards.
- **No saved setup:** the first type in `platform::preferredAudioDeviceTypes()` that opens is used, with its default output device. Windows prefers WASAPI exclusive, then Low Latency, then shared. Linux prefers JACK, then ALSA.
- **Startup stages logged:** app init, settings loaded, engine created, audio open started, native window created, content created, window laid out, window visible, window created, window shown, audio started, ready. Times are measured from the moment the OS created the process. `--quit-when-ready` exits after "ready", for `scripts/perf-check.ps1`.
- **Docker:** LLVM 21 from apt.llvm.org (≥ 20 is required). Builds go in a named volume (`lattice-build`), and ccache plus the CPM cache go in another (`lattice-cache`), so nothing is written to the source tree. `scripts/linux-test.sh` also works from Git Bash.
- **Tests:** 30 Catch2 cases. Tags become ctest labels, and the test presets leave out the heavy tags.

## Not in M0
MIDI input, the tuning system, any project model, and plugin hosting.
