# Reference Machine

Every performance target in `../09-performance-charter.md` is measured and enforced on this machine.

## Hardware (recorded 2026-10-11)
| Part | Spec | What it means for Lattice |
|------|------|---------------------------|
| Model | Lenovo Legion Pro 16ACH6H (laptop) | Laptop power and thermal behavior affect results. See "Test conditions" |
| CPU | AMD Ryzen 7 5800H (Zen 3), 8 cores / 16 threads, 3.2 GHz base (up to about 4.4 GHz boost) | **8 physical cores**: the parallel engine (M3) uses the audio thread plus up to **7 worker threads** by default |
| CPU instruction sets | SSE4.2, AVX, **AVX2**, FMA. **No AVX-512** (Zen 3 doesn't have it) | The fastest DSP code path here is **AVX2**. AVX-512 versions can't be measured or enforced on this machine; they're checked for correctness only (on CI runners that have AVX-512, or skipped) |
| RAM | 32 GB | Plenty for samples kept in memory. The 200 MB reference set is far below the limit |
| GPU | NVIDIA GeForce RTX 3070 Laptop (8 GB), plus integrated AMD Radeon | Direct2D drawing (M4). With hybrid graphics, the app may draw on the integrated GPU. Frame-time targets are measured in the mode recorded below |
| OS | Windows 11 Home 26H2, build 26300.9457, x64. Virtualization-based security is on (Docker Desktop/WSL2) | VBS can add DPC latency and stops LatencyMon from working; see the latency check below |
| Storage | 2 × WD PC SN730 1 TB NVMe SSD | Set-loading and startup targets assume an SSD |
| Audio | **Reference output: Schiit Magni Unity** (headphone amp with a USB DAC; Windows names it "Speakers (Magni Mesh Dac)"). It uses Schiit's Unison USB, which is driverless (Windows' USB Audio 2.0 class driver), with no vendor ASIO driver. Also built in: Realtek ALC3306 (HD Audio). No audio interface with inputs | See "Audio setup" below. The 5 ms total pad-to-sound target needs a device that can run buffers under 3 ms. Results above that floor are **provisional** |

## Audio setup
- **Default: WASAPI exclusive mode** on the Realtek chip. It's built into Windows and JUCE supports it directly, with nothing extra to install.
- **Alternative: ASIO4ALL** (and FlexASIO, an open-source alternative). These need Lattice built with ASIO support (`LATTICE_ENABLE_ASIO`, on by default on Windows since 2026-10-11).
  - In M0, measure **ASIO4ALL against WASAPI exclusive** on this laptop: the smallest buffer with zero dropouts over 10 minutes, and the latency each reports.
  - Use whichever is better from then on, and record the choice below.
- **Note:** both take exclusive control of the device, so other apps go silent while Lattice plays.
- **Provisional targets on built-in audio:**
  - The smallest stable buffer is expected around 128–256 samples (possibly 64).
  - Total pad-to-sound is expected around **8–12 ms**.
  - **Lattice's own share of the latency (charter §1) is enforced in full regardless of the audio hardware.**
- **If an interface is added later** (e.g. MOTU M2, Focusrite Scarlett 2i2 4th gen, Audient EVO 4), record it here. From then on the 5 ms total target is enforced.
- **The Magni Unity in WASAPI exclusive mode has a fixed floor of 3 ms.** At 48 kHz the smallest buffer is 144 samples. At 88.2 kHz it ignores a 144-sample request and runs 265 samples. Either way, the reported output latency is 6.0 ms. **A 64-sample buffer (1.33 ms) can't be reached through WASAPI on this device.** JUCE's "available buffer sizes" list for WASAPI exclusive is generic and doesn't show this; the log line records the size actually used.
- **M0 comparison (2026-10-11, Magni Unity, 48 kHz, 10-minute soaks, Performance mode, AC power):**

  | Path | Smallest stable buffer | Reported output latency | Pad-to-sound estimate* | Soak |
  |------|------------------------|-------------------------|------------------------|------|
  | WASAPI exclusive | 144 samples (3.00 ms), the device floor | 6.00 ms | ~6–9 ms (avg ~7.5) | 200,043 callbacks, 0 overruns, 0 xruns, max load 6.2% |
  | ASIO4ALL 2.22 (Magni only) | 64 samples (1.33 ms) | 7.62 ms | ~7.6–9 ms (avg ~8.3) | 450,098 callbacks, 0 overruns, 0 xruns, max load 14.9% |
  | FlexASIO 1.10b (default config: shared WASAPI) | 480 samples (10 ms) | 30 ms | n/a | dropped: set to exclusive it only reaches the same WASAPI path Lattice already has |
  | ASIO4ALL → Realtek (for reference) | 64 samples (1.33 ms) | 3.23 ms | ~3–4.5 ms | 30 s only, 0 dropouts |

  \*A MIDI event waits up to one buffer, and then the output latency applies. Latencies are as reported by the drivers; a loopback measurement (planned with the pad-to-sound tooling) would replace them with measured values.
- **Chosen default: WASAPI exclusive.** ASIO4ALL's smaller buffer is cancelled out by its extra kernel-streaming buffers (higher reported output latency), it's a third-party driver, and it caused an exit hang (below). ASIO4ALL stays selectable in the device menu.
- **ASIO4ALL exit hang (2026-10-11):** in a session where ASIO4ALL's panel switched its device from the Realtek to the Magni during playback, JUCE handled the driver's reset request (close, release, reopen) and the run finished normally, but the process then got stuck while exiting: `asio4all64.dll` was still loaded, and its one remaining thread sat in a kernel wait. It can't be killed, and replugging the Magni didn't clear it; it needs a reboot. It doesn't block new instances. It didn't happen in runs without a mid-session device switch (5 clean ASIO4ALL runs, including the 10-minute soak). Advice: change ASIO4ALL's device setup only while no program is using it. `perf-check.ps1` ignores such leftover processes (with a warning).
- **Notes for the 5 ms total target:** the Realtek through ASIO4ALL reports about half the Magni's latency, but it isn't the user's listening device. Reaching 5 ms total on the Magni needs a lower-latency path than either driver gives today; an audio interface with its own ASIO driver remains the way to get there.

## Test conditions (keep these the same for every measurement)
Gaming laptops vary a lot between runs unless these are fixed:
- **Plugged in to AC power.** On battery, the CPU slows down.
- **Lenovo power mode: Performance** (Fn+Q, or Lenovo Vantage/Legion Space), and the Windows power plan on **Best performance**.
- **GPU mode:** record which is used (Hybrid, or the dedicated GPU only through the MUX switch). Keep it the same between runs.
  - Recorded 2026-10-11: Windows lists only the NVIDIA GPU (driver 32.0.16.1692), so the MUX appears to be set to **dedicated GPU only**.
- Close heavy background apps (browsers, game launchers, sync tools), and let the laptop sit on the desktop for 1 minute before measuring.
- Don't run tests while the laptop is already hot from earlier heavy work. Benchmarks run 3 times, and the **median** is used.

## Laptop audio-latency check (do this once before M0 sign-off)
Gaming laptops often have **driver latency spikes** (DPC/ISR) from network, GPU, or ACPI drivers. These cause audio dropouts no matter how fast Lattice is.
1. Run **LatencyMon** for 10 minutes while the machine is idle and again while it's busy.
2. If it reports problem drivers, the usual fixes on this kind of laptop are:
   - update or disable the Wi-Fi/Bluetooth driver during sessions
   - update the NVIDIA driver (or try the dedicated GPU-only mode)
   - update the BIOS and chipset drivers
   - turn off CPU core parking (power plan)
3. Record the result here: the highest DPC and ISR times, and what was changed.

**2026-10-11:** LatencyMon 7.31, run as administrator, crashes at start ("Access violation at 00F7842B") on this Windows 26H2 build. Virtualization-based security is on, which is a known problem for LatencyMon. Not measured yet. The Lattice soak (0 dropouts over 10 minutes at a 3 ms buffer, Performance mode, Docker Desktop running) shows that the drivers don't cause dropouts at that buffer size. Retry LatencyMon with Docker Desktop quit (`wsl --shutdown`) or with a newer version, and in any case if a dropout ever appears.

The 0-dropouts targets are judged **after** this check. A dropout caused by a system driver is logged as an environment problem, not a Lattice regression, but it still has to be fixed or worked around before a milestone sign-off.

## Results log
| Date | Milestone | Commit | Notes |
|------|-----------|--------|-------|
| 2026-10-11 | M0 | (m0-skeleton branch) | `scripts/perf-check.ps1`, Release build, median of 3: **window shown 0.367 s** (target 0.5), **ready 1.269 s** (target 1.5). Device: Realtek, WASAPI exclusive. Conditions: on the **"Legion Balance Mode"** power plan, not Performance mode, so not valid for sign-off. See "M0 startup findings" below |
| 2026-10-11 | M1 | c8bcec6 (m1-tuning-core) | Performance mode, AC power, Magni (WASAPI exclusive). Startup median of 3: **window shown 0.381 s**, **ready 0.537 s**. Loading the tunings (12-TET + Colundi, BinaryData parse) takes under 1 ms. Building the window contents, including the new play panel, takes about 1 ms. Native window creation (~0.26 s) is still the largest stage. A run on Balance mode the same evening gave 0.48 s / 0.79 s, which shows how much the power plan matters |
| 2026-10-11 | M2 | (m2-engine-session branch) | Engine benchmark, Release, `lattice_engine_bench --seconds 20 --block 64`: the M2 reference set (32 test-synth tracks, scene 1 playing) renders at **8.5× real time** on one thread; per-block load p50 10.5%, p99 24.0%, **p99.9 34.0%** of the 1.33 ms period (budget 70%). Conditions not controlled (power plan unknown, Docker building in the background); the instruction-count baseline (`docs/perf/baselines/engine-callgrind.txt`, 2.17 G instructions for 2 s) is the stable measure |
| 2026-10-11 | M0 | (m0-skeleton branch) | ASIO comparison (see "Audio setup"): ASIO4ALL → Magni at 64 samples, 10 min, 0 dropouts, 7.62 ms reported output latency. WASAPI exclusive kept as the default |
| 2026-10-11 | M0 | (m0-skeleton branch) | Proper conditions (Legion Performance Mode, AC power). Device: Magni Unity, WASAPI exclusive. Startup median of 3: **window shown 0.360 s**, **ready 0.525 s** (the Magni opens in exclusive mode much faster than the Realtek). **Soak** (`perf-check.ps1 -Soak -SampleRate 48000 -BufferSize 144`): 600 s, 200,043 callbacks, **0 overruns, 0 xruns**, callback load p50/p99/p99.9 0.5%, max 6.2% of the 3 ms period (budget 70%). PASS |

## M0 startup findings (2026-10-11)
- **The native window takes about 0.26 s to create** (JUCE's window and Direct2D setup). Everything else before the first frame takes under 0.05 s. Revisit in M4 if the UI makes the window heavier.
- **Opening the audio device depends on the WASAPI mode** (Realtek ALC3306, 44.1/48 kHz, 512-sample buffer):

  | Mode | Device open time |
  |------|------------------|
  | Windows Audio (shared) | ~0.17 s |
  | Windows Audio (Low Latency Mode) | ~0.17 s |
  | Windows Audio (Exclusive Mode) | ~1.2 s |

  In exclusive mode, JUCE asks the driver about every sample rate and format (`IsFormatSupported`), and the Realtek driver answers slowly. With the device opened on the message thread after the first frame, "ready" was about 1.9 s (fail). With the device opened on a background thread, starting while the window is built, it's about 1.27 s (pass, but the slowest run was 1.508 s, so the margin is thin).
- The WASAPI exclusive vs ASIO4ALL comparison should also record the open times. If exclusive mode stays the default, consider patching or caching JUCE's rate probe if the margin becomes a problem.
