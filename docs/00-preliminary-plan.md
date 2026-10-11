# Lattice: Preliminary Plan and Scope

## Context
Goal: build a desktop DAW that works as much like Ableton Live as possible. It should be playable from a Push-style hardware controller and treat the **Colundi scale** as a core tuning system. The project folder is empty except for `colundi_scale.txt`, so this is a new build. This document sets the architecture, scope, and milestone order. Each numbered component below will get its own detailed sub-plan.

**Decisions so far**
- Name: **Lattice** (chosen 2026-10-11). Repo: `lattice-app/`.
- Stack: **C++20 + JUCE 8**, built with CMake.
- Platforms: **Windows is the main platform** and the performance reference. **Linux is second-tier from M0**: built and tested in Docker and CI on every commit. macOS comes later. See "Platforms" below.
- MVP: **Session view + built-in instruments + Push control.** Arrangement view, audio recording, and warping come after the MVP.
- Controllers: a **controller abstraction layer** for Push-style devices and generic MIDI controllers.
  - **Ableton Push 2/3** gets a native driver in M5, because Ableton publishes its MIDI and display protocol (github.com/Ableton/push-interface).
  - Other controllers work through JSON surface definitions plus a learn wizard (M5).
  - **The user's own controller isn't chosen yet.** The recommendations are a used Push or a Launchpad Pro MK3. Whichever is bought gets a native driver, built together on the real device in M7.
- Colundi applies everywhere: built-in instruments, Push pad layout, external plugins (MTS-ESP, .scl/.tun export), and external MIDI hardware (MPE / per-note pitch bend).
- **Hardware is optional.** The app runs fully on one computer with a mouse and keyboard and needs no internet. See "Design principle" below.

## Priorities: quality and performance first
The user confirmed this on 2026-10-11: **sound quality and performance come before schedule.** There's no deadline, so polish can wait. In practice:
- **Quality over shortcuts:** when choosing a DSP technique, use the higher-quality one, as long as it fits the CPU budget. Examples:
  - band-limited oscillators
  - sinc sample interpolation
  - our own high-quality reverb
- **Performance is measured, not assumed:**
  - Every device and engine path has a CPU budget, tracked with benchmarks.
  - A regression counts as a failure, not just a warning.
  - The engine processes tracks in parallel.
- **No hard-coded hardware sizes:**
  - Pad grids, encoder counts, macro counts, and parameter page sizes are all settings, never fixed limits.
  - "8" only appears as the default page size of an 8-encoder controller.

## Platforms
| Tier | Platform | What that means |
|------|----------|-----------------|
| 1 | **Windows 10/11** | Main development platform. All performance budgets and latency targets are enforced on the reference machine (the user's PC). Hardware is tested here first |
| 2 | **Linux** (Ubuntu 24.04 LTS as the reference distro; x86-64) | Builds and runs the full test suite on every commit (CI plus local **Docker**). The plans give every feature a Linux version. Performance is measured but not enforced until a real Linux machine is available. Manual checks are done when hardware allows |
| 3 | **macOS** | Kept in mind in the design (JUCE, and abstractions that include it). Built in CI where it's cheap. Not tested beyond that until after the MVP |

### Rules for code that differs per platform
- **All platform-specific code lives in `src/platform/`** behind small interfaces, with one implementation per OS. Feature code never uses `#ifdef` on the OS. Each interface has a Windows and a Linux implementation (and a macOS one where it's cheap):

| Interface | Windows | Linux |
|-----------|---------|-------|
| Real-time thread priority | MMCSS "Pro Audio" | `SCHED_FIFO` through rtkit, or `pthread` if the user's limits allow it |
| Timed MIDI out | Windows MIDI Services, or a high-priority sender thread | ALSA sequencer queue (scheduled by timestamp) |
| Cross-process signaling (plugin isolation) | named events and shared memory | futex / POSIX semaphores and `shm_open` |
| USB access to controllers | libusb + WinUSB | libusb / hidraw, plus **udev rules** shipped with the app |
| File watching | `ReadDirectoryChangesW` | inotify |
| Crash dumps (kept on the computer) | minidumps | a Breakpad/Crashpad-style handler (local only) |
| GPU drawing for heavy views | Direct2D (JUCE 8) | OpenGL (`juce::OpenGLContext`) |
| Installer | WiX or Inno Setup | `.deb` plus AppImage (both install the udev rules) |

- **User data folder `<userdata>`:** `%APPDATA%/Lattice` on Windows, `$XDG_DATA_HOME/Lattice` (default `~/.local/share/Lattice`) on Linux, `~/Library/Application Support/Lattice` on macOS. Settings go in `%APPDATA%/Lattice` and `$XDG_CONFIG_HOME/Lattice` respectively.
- **Audio back ends:** WASAPI and ASIO (optional) on Windows; ALSA and JACK on Linux (PipeWire works through its JACK or ALSA compatibility).

### Linux testing with Docker (keeping compute low)
- **The image:** `docker/linux.Dockerfile` (Ubuntu 24.04) contains the compilers, CMake, Ninja, ccache, JUCE's Linux build dependencies, Xvfb, and the locales the tests need.
- **The scripts** `scripts/linux-test.sh` and `scripts/linux-test.ps1` do the following:
  1. Build the image once.
  2. Mount the repo, with a **ccache volume** so rebuilds are fast.
  3. Run CMake configure, build, and `ctest`. UI snapshot tests run under `xvfb-run`.
- **Keeping it light:**
  - **Test tags:** `[slow]`, `[bench]`, `[stress]`, `[hardware]`, `[timing]`. The default Docker run **leaves out** all of them, and they run in a nightly CI job or on demand.
  - **Targets:** the default run takes **under 5 minutes** after a cached build, and uses at most 4 cores.
- **What Docker can't test:** real audio devices, real MIDI ports, USB controllers, or real-time scheduling. Tests that need them are tagged `[hardware]` or `[timing]` and skipped in Docker. Everything else uses the offline renderer and fake devices, which run the same in Docker.
- **CI:** GitHub Actions runs Windows, Ubuntu (using the same Dockerfile), and macOS (build plus fast tests). Nightly jobs run the tagged tests where runners allow.

## Design principle: everything works without hardware
- **Every controller action has a mouse/keyboard equivalent in the app.** This covers launching, Note mode, editing device settings, mixing, browsing, and the Colundi pad layout. Controller drivers only add to the app's features and never own a feature.
- Rule for every sub-plan: a feature isn't done until it can be used without a controller connected.
- Tools that make this possible:
  - **Computer MIDI keyboard (M1):** play notes with the letter keys. The keys step through the Colundi table when Colundi is the active tuning.
  - **On-screen keyboard (M1)** and any USB/MIDI keyboard.
  - **Virtual Push (M5):** an on-screen pad grid with mode buttons and encoders.
    - It is built as just another `ControlSurface` driver, so it runs the same mode logic as real Push hardware, including the Colundi layout. It lets the Push modes be built and tested without hardware.
    - It can be used with a mouse or a touchscreen.
    - **Its size can be changed:** any grid from 4×4 up to 16×16 (e.g. 8×8, 8×12, 16×16), and 8, 16, or 32 encoders.

## Colundi scale: what the file tells us
`colundi_scale.txt` is tab-separated with columns `# | Freq | Cents | Ratio | Name`.
- **It has exactly 128 entries (0–127)**, so it maps one-to-one onto MIDI notes. It is an absolute frequency table, not an octave-repeating scale.
- The base is 30 Hz. Ratios are relative to 30 Hz and are mostly just-intonation-like (16/15, 11/10, 7/6, 4/3, 3/2, 7/4…). It **does not repeat by octave**: index 12 is 59.9 Hz, not 60.
- Parsing problems to handle:
  - The Cents column switches to scientific notation (`1.013e+4`) above 10000.
  - The Name column uses comma decimals (`1,06666`).
  - Treat the **Freq column as the source of truth**.
- Rows 0 and 120–127 look like extrapolations by a period of 938.667. Their Name column wraps back to the start of the table, and rows above about 20 kHz are past Nyquist at 44.1/48 kHz. They need clamping or muting.
- Design consequence: the tuning engine must be built on **per-note absolute frequency tables** (with octave-based scales as a special case), not on "12 notes + period".

## Architecture (components and sub-plans)

| # | Component | What it covers |
|---|-----------|----------------|
| 1 | **Core / Project model** | Document tree (Set → Tracks → Clips/Devices), value-tree based state, undo/redo, save/load (`.lset` JSON/XML), sample and preset paths |
| 2 | **Audio engine** | Real-time audio graph, lock-free UI↔audio messaging, transport/tempo/time signature, latency compensation, WASAPI/ASIO (Windows), ALSA/JACK (Linux), CoreAudio (macOS) through JUCE |
| 3 | **Clip and session engine** | MIDI clips, scenes, launch quantization, legato and launch modes, follow actions, global groove/swing, metronome, tap tempo |
| 4 | **Tuning system (Colundi)** | Tuning table service (parse Colundi txt, .scl/.kbm/.tun import), per-track tuning selection, frequency lookup API used by all instruments, MTS-ESP master, MPE/pitch-bend retuning for external MIDI |
| 5 | **Built-in devices** | Instruments: subtractive/wavetable synth, Simpler-style sampler, Drum Rack. Effects: EQ, compressor, reverb, delay, filter, saturator, utility. Instrument/Effect Racks with a **configurable number of macros (0–64, default 8)**; controllers page through them by their own encoder count |
| 6 | **Plugin hosting** | VST3, CLAP (JUCE if it can host CLAP, otherwise our own host), AU (macOS), LV2 (Linux, optional); plugin scanning in a separate process, crash isolation, parameter exposure to the controller layer |
| 7 | **UI** | Session view grid, device chain view, MIDI clip piano-roll (with a Colundi-aware note ruler that shows Hz instead of note names), mixer, browser, preferences (audio/MIDI/controllers). Visual style follows Live (flat, dense, keyboard-driven) |
| 8 | **Controller layer** | Abstract `ControlSurface` API (pads, buttons, encoders, touch strip, display, LEDs).<br>• Each surface **declares its own capabilities**: grid size, number of encoders, display size.<br>• The modes (Session, Note, Device, Mix, Browse) are written against those capabilities, never against a fixed 8×8 or 8 encoders. Drivers:<br>• **Virtual Push** (on-screen, always available)<br>• **Push 2/3:** MIDI plus 960×160 display over libusb (WinUSB on Windows, a udev rule on Linux)<br>• **Generic MIDI controllers:** JSON surface definitions plus a learn wizard<br>• **The user's controller:** a native driver in M7 |
| 9 | **MIDI I/O and external hardware** | Device routing, MIDI clock in/out, External Instrument device, MPE output, Colundi retune output |
| 10 | **Post-MVP** | Arrangement view, audio tracks and recording, warping/time-stretch, automation lanes, freeze/bounce, export/render, Link sync |

### Proposed repository layout
```
lattice-app/
  CMakeLists.txt            # JUCE through CPM/FetchContent
  src/core/                 # project model, undo, serialization
  src/engine/               # audio graph, transport, session/clip engine
  src/tuning/               # tuning tables, Colundi parser, MTS-ESP, MPE retune
  src/devices/              # built-in instruments and effects, racks
  src/hosting/              # VST3/CLAP host and scanner
  src/control/              # ControlSurface API, modes, drivers (virtual/, push/, generic/, native/)
  src/ui/                   # JUCE components for each view
  src/platform/             # per-OS implementations (windows/, linux/, macos/) behind shared interfaces
  resources/tunings/colundi_scale.txt
  resources/linux/udev/     # udev rules for controller USB access
  tests/                    # Catch2 unit tests and offline render tests
  docker/linux.Dockerfile   # Linux build and test image
  scripts/                  # linux-test.sh / .ps1 and other dev scripts
  docs/
```

## Push / Colundi interaction design (summary)
- **Note mode, Colundi layout**: the pads (8×8 on Push, any size on Virtual Push) walk the 128-entry table in order. Each row shifts by a configurable offset (default 5, similar to Push's in-key 4ths layout). The octave buttons step by 8 or 12 indices, or by "period".
- Pad colors mark anchor notes (exact multiples of 30 Hz, e.g. 60/90/120/240 Hz) separately from other notes.
- The display shows the note index and its Hz value instead of a note name. An encoder selects the active tuning per track.
- The same layout works on 4×4 controllers such as Maschine MK3 (16-step pages), on generic grid controllers, and on the on-screen Virtual Push.

## Milestones (sequenced)
1. **M0 Skeleton**: CMake + JUCE app, audio device settings, a sine-wave test tone, CI build, Catch2 tests.
2. **M1 Tuning core**: Colundi parser and tuning service, with unit tests checking all 128 frequencies. Test synth plays from the table (first audible Colundi result), played from the on-screen keyboard, the computer keyboard, or MIDI input.
3. **M2 Engine + session**: transport, MIDI tracks, MIDI clips, scene/clip launching with quantization, basic mixer.
4. **M3 Built-in devices**: synth, sampler, drum rack, MIDI and audio effects, racks/macros, return tracks/sends, sidechain, delay compensation, presets.
5. **M4 UI**: session view and mixer, group tracks, Clip view with the Colundi piano roll, device panels, browser and library, tuning tools, file handling and crash recovery, preferences, MIDI/Key Map mode.
6. **M5 Controllers**: the mode logic (Session, Note with the Colundi layout, Drum, step sequencer, Device, Mix, Browse, Clip) works on any grid or encoder count. It is built and tested first against the on-screen Virtual Push. Then come the Push 2/3 drivers (MIDI, display, MPE), **generic MIDI controllers** through JSON surface definitions plus a learn wizard, and a low-latency path for pad notes.
7. **M6 Plugin hosting + external retune**: VST3/CLAP/AU hosting with scanning in a separate process and optional crash isolation; MTS-ESP master; .tun/.scl/.kbm/MTS sysex export; hardware retuning by MPE bend or MTS sysex; External Instrument with measured latency; MIDI clock in/out; Tuning Verifier.
8. **M7 Native drivers + finishing the MVP**: native drivers for whichever controller(s) the user buys (Push 2/3, Launchpad Pro MK3, APC64, APC40 MKII, or Maschine MK3), built and checked together on the real device. Then the end-to-end acceptance test, an 8-hour soak test, the installer, and a licensing review. **MVP complete.**
9. **M8+ Post-MVP**: arrangement view, audio tracks, warping, automation, export.

## Key risks
- **Real-time safety**: no allocation or locks on the audio thread. Enforce this from M0 with lock-free FIFOs and atomic parameter snapshots.
- **Push display on Windows** needs the WinUSB driver (Ableton Live installs it). Detect it and give a clear message if it is missing.
- **The controller hasn't been chosen yet.** M5 keeps every candidate supported, and M7 works for whichever device is bought. Maschine MK3 is the riskiest candidate because its protocol isn't public.
- **MTS-ESP** uses the ODDSound SDK (free to use; check the license terms). Not every plugin supports it, so .scl/.tun export is the fallback.
- **Pitch-bend retuning** of external hardware needs MPE or one note per channel, and the bend range must match the synth (±48 semitones is the MPE default).
- **"As similar as possible to Live"**: copy the workflows and concepts, not Ableton's trademarks or assets.
- **Linux:**
  - UI drawing speed (JUCE draws in software on Linux, so heavy views use OpenGL).
  - Real-time priority depends on the user's system settings (the app detects this and explains the fix).
  - Running on Wayland goes through XWayland (fractional scaling may look soft).
  - Multi-touch on Virtual Push may be mouse-only at first.
  - Fewer Linux plugins exist.
  - Whether MTS-ESP is available on Linux needs checking.
  - Docker can't test real audio, MIDI, or USB, so those parts on Linux are only checked once a Linux machine is available.

## Verification (per milestone)
- Unit tests (Catch2): the tuning parser checks every row's frequency within 0.001 Hz and the edge rows (sci-notation cents, comma names). Also clip launch quantization timing and serialization round-trips.
- Offline render tests: render a MIDI clip through the engine with no audio device, then FFT the output to confirm each note's fundamental matches its Colundi frequency.
- No-hardware check at every milestone: with no controller connected, every new feature can be used with the mouse and keyboard.
- Manual: launch clips from Push, play the Colundi layout on the pads and hear the right pitches, check the Push display. MTS-ESP: load a supporting synth (e.g. Surge XT, which is free) and confirm it follows the tuning.
- Real-time safety: build in debug mode with an allocation/lock detector hook on the audio thread.
- **Linux at every milestone:** the default Docker run (`scripts/linux-test.ps1`) passes, and the Ubuntu CI job is green. A milestone isn't done if Linux is broken.

## Sub-plans
Sub-plans are written in milestone order. See `docs/README.md` for the index.
- `01-m0-skeleton.md`: M0 Skeleton
- `02-m1-tuning-core.md`: M1 Tuning core (component #4, first part)
- `03-m2-engine-session.md`: M2 Engine + Session (components #1–3)
- `04-m3-devices.md`: M3 Built-in devices (component #5, plus returns/sends, sidechain, delay compensation)
- `05-m4-ui.md`: M4 UI (component #7, plus group tracks, the library index, and MIDI/Key Map mode)
- `06-m5-controllers.md`: M5 Controllers (component #8: Virtual Push, Push 2/3, generic MIDI controllers)
- `07-m6-plugins-external.md`: M6 Plugin hosting, external hardware, and retuning outside the app (components #6, #9, and the rest of #4)
- `08-m7-native-drivers.md`: M7 Native drivers for the purchased controller(s), and finishing the MVP
