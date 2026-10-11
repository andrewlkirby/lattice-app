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
| OS | Windows (64-bit, x64); record the exact edition and build here: | |
| Storage | (record the drive model and whether it's NVMe) | Set-loading and startup targets assume an SSD |
| Audio | **Built-in Realtek ALC3306** (HD Audio). No external interface yet | See "Audio setup" below. The 5 ms total pad-to-sound target needs a dedicated interface with an ASIO driver. On built-in audio, total-latency results are **provisional** |

## Audio setup
- **Default: WASAPI exclusive mode** on the Realtek chip. It's built into Windows and JUCE supports it directly, with nothing extra to install.
- **Alternative: ASIO4ALL** (and FlexASIO, an open-source alternative). These need Lattice built with ASIO support (`LATTICE_ENABLE_ASIO`, once the ASIO SDK license is confirmed in M0).
  - In M0, measure **ASIO4ALL against WASAPI exclusive** on this laptop: the smallest buffer with zero dropouts over 10 minutes, and the latency each reports.
  - Use whichever is better from then on, and record the choice below.
- **Note:** both take exclusive control of the device, so other apps go silent while Lattice plays.
- **Provisional targets on built-in audio:**
  - The smallest stable buffer is expected around 128–256 samples (possibly 64).
  - Total pad-to-sound is expected around **8–12 ms**.
  - **Lattice's own share of the latency (charter §1) is enforced in full regardless of the audio hardware.**
- **If an interface is added later** (e.g. MOTU M2, Focusrite Scarlett 2i2 4th gen, Audient EVO 4), record it here. From then on the 5 ms total target is enforced.
- Results: smallest stable buffer, with WASAPI exclusive: ___ / with ASIO4ALL: ___ / chosen: ___

## Test conditions (keep these the same for every measurement)
Gaming laptops vary a lot between runs unless these are fixed:
- **Plugged in to AC power.** On battery, the CPU slows down.
- **Lenovo power mode: Performance** (Fn+Q, or Lenovo Vantage/Legion Space), and the Windows power plan on **Best performance**.
- **GPU mode:** record which is used (Hybrid, or the dedicated GPU only through the MUX switch). Keep it the same between runs.
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

The 0-dropouts targets are judged **after** this check. A dropout caused by a system driver is logged as an environment problem, not a Lattice regression, but it still has to be fixed or worked around before a milestone sign-off.

## Results log
| Date | Milestone | Commit | Notes |
|------|-----------|--------|-------|
| | | | |
