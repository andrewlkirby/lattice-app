# Performance Charter

This applies to **every milestone** (M0–M7 and later). Parent: `00-preliminary-plan.md`, "Priorities".

Lattice has to **load fast and feel immediate to play**. This charter defines what "fast" means as measurable targets. It also sets out how each target is measured, the gates every change goes through, and the coding and build rules that keep the app fast by design.

**The rule:** a milestone isn't done until its performance gate passes. A change that makes a target worse is a bug, even if every feature test passes.

## 1. Targets
All targets are measured on the **reference machine**: the user's Lenovo Legion Pro 16ACH6H (Ryzen 7 5800H, 8 cores, AVX2, 32 GB), described in [`perf/reference-machine.md`](perf/reference-machine.md) together with the fixed test conditions. Unless a row says otherwise, the setup is 48 kHz with a **64-sample buffer**, on ASIO (or WASAPI exclusive).

### Feel when playing (the most important)
| Target | Value | Notes |
|--------|-------|-------|
| Buffering added by Lattice | **None** beyond the driver's buffers | No hidden internal buffer or extra block of delay. The only exceptions are delay compensation (which the user can see) and Pipelined plugin isolation (which the user chooses) |
| Pad / key press → sound, the part Lattice controls | **≤ 1 buffer + 1 ms** | Measured from the MIDI input timestamp to the sample being handed to the driver. At 64 samples that's about 2.3 ms on top of the driver's own latency |
| Pad / key press → sound, total | **≤ 5 ms** at 64 samples on the reference audio interface | Includes the driver and converter latency the interface reports (measured as in M6) |
| Monitoring an armed track | With **"Reduced latency when monitoring"** turned on, armed tracks skip delay compensation, as in Live | New in this charter. Implemented in M3 phase 3e |
| Audio callback headroom | Worst-case processing **≤ 70% of the buffer period** at p99.9; **0 dropouts** | In the 32-track reference set (defined below) |
| MIDI out jitter | **≤ 1 ms** p99 | M6 |
| Controller LED/display response | **≤ 1 display frame** after the state change | M5 |

### Loading
| Target | Value |
|--------|-------|
| Cold start → window visible | **≤ 0.5 s** |
| Cold start → app ready to use (empty set, audio running) | **≤ 1.5 s** |
| Opening the **reference set**: playable | **≤ 3 s** (from an SSD) |
| Opening a set with plugins | Plugin instances load **in parallel**. Nothing ever waits on a plugin scan |
| Loading a sample | Never blocks the UI. A 100 MB WAV is playable in ≤ 1 s |
| Saving | In the background. **No UI hitch longer than 1 frame**, even for large sets |

### UI responsiveness
| Target | Value |
|--------|-------|
| Click, key press, or encoder turn → visible response | **≤ 1 frame** (16.7 ms at 60 Hz) |
| Frame time while busy | **≤ 8 ms** (M4 scenarios) |
| UI activity causing audio dropouts | **0** (M4 stress test) |

### Resources
| Target | Value |
|--------|-------|
| Idle CPU (transport stopped, nothing moving on screen) | **< 1%** of one core; meters and animations stop updating when nothing changes |
| Memory | Samples are decoded into RAM once and shared between devices. Any increase in memory not explained by content is a bug (checked in the M7 soak test) |

### The reference set (kept in `tests/perf/reference-set/`)
- **Tracks:** 32 tracks. 16 Synth (8 voices active), 8 Drum Racks (Sinc sampler), and 8 Samplers (Sinc).
- **Effects:** each track has EQ Eight, Compressor, and Saturator (2×). 2 returns: Reverb High and Delay. A Limiter on master. 4 sidechains.
- **Samples:** about 200 MB.
- **Plugins:** from M6, plus 6 of the test plugins.

It grows as features arrive, and every milestone measures against the version current at that time.

## 2. Measuring
- **Built-in measurements** (in every build, very low cost):
  - **Audio callback timing:** a lock-free histogram per callback (p50/p99/p99.9/max as a share of the buffer period), plus a dropout counter and the time per worker thread. Shown in the CPU meter's hover panel (M4) and written to the log.
  - **Startup timeline:** each startup stage is timestamped and written to `<userdata>/logs/startup.log` (window shown, audio started, library ready, set ready).
  - **Input latency probe:** samples the time from MIDI input to output. A debug panel shows it.
  - **Per-device CPU** (M6), and frame time (M4).
- **Profiler:** **Tracy** (BSD license) is built in through the `LATTICE_PROFILE` build option. Its markers compile away to nothing in normal builds. It's used to look into any regression.
- **Steady numbers on any machine:** on Linux in Docker, important code paths are measured by **instruction count** (Valgrind `callgrind`). Instruction counts don't depend on how fast or busy the machine is, so they can catch slowdowns on shared CI machines.
- **The `lattice_bench` target:** micro-benchmarks for each device and each engine path (M3). Baselines are stored in `docs/perf/baselines/`.

## 3. Gates

| Gate | When | What runs | Fails on |
|------|------|-----------|----------|
| **Every commit** (CI + Docker, cheap) | Every push | RealtimeCheck in Debug tests; **RTSan**, Clang's real-time sanitizer (`-fsanitize=realtime`, which needs **Clang 20 or newer**), on the Linux Clang build, which marks audio-thread entry points `[[clang::nonblocking]]`; `clang-tidy` `performance-*` checks; allocation-count tests (e.g. "processing 10,000 blocks allocates 0 bytes") | Any allocation, lock, or blocking system call on a real-time path. Any new `performance-*` warning |
| **Nightly** (CI) | Once a day | Instruction counts for the main benchmarks; `[slow]` and `[stress]` tests | Instruction count **up by more than 5%** compared with the baseline (unless the baseline is updated, with a written reason) |
| **Reference machine** (`scripts/perf-check.ps1`) | Before merging any change that could affect performance, and at every milestone sign-off | Benchmarks against baselines; startup and set-loading timings; callback headroom in the reference set; the UI stress test; the latency probe | Any target in §1 missed, or a benchmark **slower by more than 10%** |
| **Milestone gate** | End of each milestone | The reference-machine gate, plus a short **hands-on playing check** with the user (latency feel, responsiveness, load times) | Any missed target, or the user saying it doesn't feel right. Feel counts as a real result |

A change that has a good reason to make something slower (for example a quality upgrade) updates the baseline **in the same commit**, with the measured cost and the reason written in the commit message.

## 4. Coding rules

### Audio and real-time threads (audio callback, workers, MIDI input fast path)
- **Never** allocate, free, lock, wait, do file/network/console I/O, throw exceptions, or make system calls that can block. RealtimeCheck and RTSan enforce this.
- **Shared data:**
  - Read immutable snapshots through **raw pointers** taken from `RtShared`.
  - Never copy a `shared_ptr` on the audio thread; its atomic reference counting causes contention.
  - Use no `std::function`, and no virtual calls **per sample**. Virtual calls once per block are fine.
- **Data layout:**
  - Use **structure-of-arrays** for voices and channels, so inner loops are SIMD-friendly.
  - `alignas(64)` on data written by different threads, to avoid false sharing.
  - Inner loops have no branches where possible.
- **Smoothing:** compute a parameter once per block, and smooth per sample **only while it is changing**.
- **No wasted work:** silent voices, bypassed devices, and muted tracks with no tails skip processing (each with a tail-length check, so reverb and delay tails still finish).
- **Denormals:** flush-to-zero and denormals-are-zero are set on every real-time thread.
- **Memory:** buffers used on real-time paths are **touched once after allocation**, so the operating system maps their memory before playback (no page faults in the callback). An optional "lock audio memory" setting uses `VirtualLock`/`mlock`.
- **Threads:** the audio thread gets the highest priority the platform allows (`platform::RealtimeThread`). Workers are pinned to physical cores where that helps.

### UI thread
- No disk I/O or long work. Use background jobs with progress (M4).
- Repaint only what changed. Cache expensive layers. Avoid allocating in `paint()` where possible.
- One refresh tick (`VBlankAttachment`), and **no refreshing when nothing changes**, so the idle CPU target holds.

### Startup and loading
- **Show the window first, then do the rest in parallel:**
  - open the audio device on a background thread
  - read the library index and the plugin list from cache (never scan at startup)
  - unpack factory content only when it's first used
- **Opening sets:**
  - Parse the set, build the engine snapshot, then load samples and plugins **in parallel** on the job pool, with progress shown.
  - Tracks play as soon as their own content is ready.
- **Do nothing up front that can wait:** device panels, browser previews, and waveform peaks are made when first needed (peaks are cached on disk).

### Data structures and algorithms
- Choose contiguous memory over linked structures.
- Use flat arrays allocated in advance on hot paths.
- Avoid repeated work: when a clip is edited, rebuild only that clip (M2).
- Big-O matters in the UI: the piano roll, the browser, and the session grid have to scale to the sizes in the M4 tests.

## 5. Build settings
- **Release:** `-O2` (GCC/Clang) or `/O2` (MSVC), with **link-time optimization** (`-flto` / `/GL /LTCG`).
- **Floating point:** no global `-ffast-math`, because it breaks the NaN/Inf checks and denormal control. Safe flags such as `-fno-math-errno` are used instead, and vectorized code is written explicitly.
- **CPU target:**
  - The baseline is **x86-64-v2** (SSE4.2), which runs on any reasonably modern PC.
  - Hot DSP kernels choose **AVX2 or AVX-512 versions at runtime** (xsimd dispatch), so one binary is both portable and fast.
- **`RelWithDebInfo` + RT checks preset** for everyday development, so audio work runs at close to real speed while debugging. Plain Debug is used only for stepping through code.
- **Profile-guided optimization:** to try in M7 (MSVC PGO / Clang PGO), using the reference set as the training workload. Kept only if it measurably helps.
- **Startup:** static initialization stays minimal (no heavy global constructors). Big tables (oscillator wavetables, sinc tables) are built **lazily or on a background thread**, never before the window appears.

## 6. Review checklist (for every change touching a hot path)
- [ ] Does any new code run on a real-time thread? If so: is it free of allocation, locks, and blocking calls? Is it marked `[[clang::nonblocking]]` where it applies? Does RTSan pass?
- [ ] Is the inner loop free of per-sample virtual calls, `std::function`, branches that could be avoided, and `shared_ptr` copies?
- [ ] Is anything computed per sample that could be computed per block?
- [ ] Does silence or bypass skip the work?
- [ ] Does the change add work at startup or when opening a set? Could it be done lazily or in parallel?
- [ ] Does the UI repaint only what changed, and stay idle when nothing does?
- [ ] Are the benchmark and instruction-count results within budget? If the change is slower on purpose, is the baseline updated with the reason?

## 7. Setting it up (milestone by milestone)
- **M0:**
  - the callback timing histogram, the startup timeline log, RealtimeCheck, and the **RTSan** Linux Clang preset in Docker
  - the Tracy option
  - the `clang-tidy` `performance-*` checks
  - the release flags (LTO, x86-64-v2), and the `RelWithDebInfo + RT checks` preset
  - `scripts/perf-check.ps1` (a skeleton; it grows with each milestone)
  - `docs/perf/reference-machine.md`
- **M1:** allocation-count tests for the tuning path. The startup cost of the tuning tables is measured.
- **M2:** the reference set starts (tracks, clips, Synth placeholders). The input latency probe. Instruction-count benchmarks for the engine.
- **M3:**
  - `lattice_bench` with device budgets
  - the full reference set
  - "Reduced latency when monitoring"
  - SIMD dispatch
  - wavetables and sinc tables built lazily
- **M4:** frame-time and idle-CPU measurement. Startup and set-loading timings measured against §1. Loading in parallel with progress.
- **M5:** pad-to-sound measured through the fast path. LED and display response.
- **M6:** total latency measured with the audio interface. MIDI jitter. Per-plugin CPU. Parallel plugin loading.
- **M7:** PGO trial, the 8-hour soak test, and the final sign-off against every target.

## 8. Linux
- Every gate above runs on Linux in Docker **except** those needing real audio or timing (the `[timing]` and `[hardware]` tags). **RTSan and the instruction counts run on Linux only**, so Linux CI is an important part of the gates, not just a side check.
- Latency and startup targets on Linux are measured, but **enforced only once a real Linux machine is available**.
