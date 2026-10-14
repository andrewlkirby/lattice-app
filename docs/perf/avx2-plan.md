# AVX2 performance plan (end of M3)

Goal: bring the five devices still over their M3 starting budgets within them, without changing
how anything sounds beyond float rounding, and keep every existing quality test passing unchanged.

| Device (lattice_bench) | Now | Budget | Main cost (callgrind) |
|------------------------|-----|--------|-----------------------|
| Synth, 16 voices × 2 osc, unison 4 | 2.7% | 2% | oscillators ~70% |
| Synth, 16 voices, ladder | 2.55% | 2% | ladder ~42%, oscillators ~30% |
| Sampler, 32 voices, sinc | 2.4% | 1.5% | sinc kernel ~74% (already AVX2) |
| Reverb, Ultra | 2.6% | 2% | tail at 2× (32 lines) |
| Reverb, Eco | 0.5% | 0.5% | input stage (early reflections, diffusers) ~50% |

**Reference machine:** AMD Ryzen 7 5800H (Zen 3): AVX2 + FMA, two 256-bit FMA units, no AVX-512.
Zen 3's `vgatherdps` is slow (microcoded), so the kernels keep today's "four loads and a transpose"
pattern instead of hardware gathers. `vdivps` gains little from 256 bits, so divisions go too.

## 1. Infrastructure (done, 2026-10-13)

- **One dispatch point:** `src/devices/dsp/Simd.h` reports the CPU's level (SSE2 or AVX2 + FMA) once,
  at startup on the message thread, and an override (`LATTICE_SIMD=sse2`) forces the SSE2 paths for
  testing and comparison. Each device picks its kernels in `prepare()`, so the audio thread never
  checks the CPU.
- **Kernel files:** AVX2 code lives only in `*Avx2.cpp` files, compiled with AVX2 + FMA and outside
  link-time optimisation (set up for the Sampler already). They include only intrinsics and
  plain-C-style headers with POD state structs, so no AVX2 copy of a shared inline function can leak
  into code for other CPUs.
- **Testing:** every AVX2 kernel keeps its SSE2 twin. A test per kernel runs both on the same input
  (random signals, block sizes 1–64, lane counts 1–33) and requires them to match within 1e-5
  relative; the device tests run with the SSE2 override as well as natively. CI's x64 runners have
  AVX2, so both paths run on every commit.
- **Measuring:** `lattice_bench --simd sse2|avx2` (and `lattice_engine_bench`) compare paths on one
  machine; the Audio tab shows the level in use. Callgrind baselines are re-recorded at the end
  (valgrind supports AVX2).
- As built: `src/devices/dsp/Simd.h` (`simdLevel()`, `setSimdLevel()`, `ScopedSimdLevel`); the
  Sampler chooses its sinc kernel in `prepare()`; CMake gives every `*Avx2.cpp` in `lattice_core` the
  flags; `tests/support/SimdPaths.h` renders at each level and compares; CTest runs the device tests
  a second time with `LATTICE_SIMD=sse2` ("Device tests on the SSE2 kernels").

## 2. Ladder filter (Synth ladder: 2.55% → about 1.6%)

- **State as plain data:** the ladder's and the oversampler's per-voice state move into POD structs
  (`LadderState`, `AllpassState`) that the C++ classes own and the AVX2 kernel reads.
- **Remove the divisions** (both paths): the three divides per tick (the tanh ratio, its 1/|a|
  branch, and the y4 solve) become `rcp` plus one Newton–Raphson step (about 23 bits, below float
  rounding of the result). Two ticks per sample at 2× means six divides per sample per four voices
  today.
- **Eight voices per register:** the ladder, its 2× all-pass oversampler and the coefficient ramp
  run over 8 lanes. Leftover lanes are padded into a full register (silent dummy lanes) instead of
  running the scalar filter, which costs four times as much per lane.
- Expected: the divisions alone take roughly a third off the ladder; 8 lanes roughly halve the
  rest.
- The SVF voice filters (the Synth's other filter types, within budget today) get the same 8-lane
  treatment since the pattern is identical.
- As built (2026-10-13): the kernels are written once (`VoiceFilterKernels.inl`, over a small
  SIMD traits type) and compiled for SSE2 in `VoiceFilterSimd.cpp` and for AVX2 + FMA in
  `VoiceFilterSimdAvx2.cpp`. The state is plain data (`VoiceFilterKernels.h`: `LadderState`,
  `HalfBandState`, `AllpassState`, `SvfStageState`), owned by `LadderFilter`, `HalfBand2x` and
  `Svf`. The Synth picks the kernels in `prepare()`.
  - **What the profile showed:** one register of lanes is limited by the length of the filter's
    feedback chain, not by the divisions (on Zen 3, `divps` has about the latency of `rcp` plus a
    Newton step). So besides removing the divisions:
    - two registers run interleaved whenever there are more lanes than one register holds
      (SSE2: 11.2 → 8.8 ns per lane-sample);
    - samples move in and out with 4 × 4 or 8 × 8 transposes instead of per-sample gathers;
    - the voice design's all-pass branches (three sections each) are unrolled, which saves
      another 10%.
  - **Kernel timings** (`lattice_tests "[bench][simd]"`, 16 lanes, ns per lane-sample):

    | Filter | SSE2 | AVX2 |
    |---|---|---|
    | Ladder | 8.0 | 4.7 |
    | SVF 24 dB | 1.5 | 1.0 |
    | SVF 12 dB | 1.1 | 0.75 |
  - **Result:** the Synth's 16-voice ladder case goes from 2.55% to 2.0% with AVX2 (2.25% with
    SSE2). The ladder itself now takes about 0.36% of a core. The rest of that case is the
    oscillators and other per-voice work, so the 1.6% target moves to step 3.
  - **Tests:**
    - every kernel against the scalar filters at each level, for 1–33 lanes and 1–64-sample
      blocks, with state carried across blocks;
    - the Synth rendered at each level must match within 1e-5.

## 3. Oscillators (Synth unison 4: 2.7% → about 1.8%)

- **Eight copies per register:** a voice's two oscillators × four unison copies (or eight copies of
  one at unison 8) in one 256-bit register. Each lane carries its own table, level and blend, so
  the two oscillators can differ in shape and pitch. AVX2's per-lane shifts (`vpsrlvd`) give each
  lane its own table size.
- **Cheaper mixing to stereo:** today every sample ends with a horizontal sum and two scalar adds.
  Instead, four samples' lanes are transposed and summed together, writing four output samples per
  step.
- **Unison 1:** `renderCopy` (one copy, four samples per step) becomes eight samples per step.
- Expected: the 8-lane kernel halves the table-reading work; the batched mixing removes about a
  fifth of the remaining per-sample overhead.
- As built (2026-10-13): the Synth describes each oscillator copy as a plain job
  (`OscillatorKernels.h`: raw table pointers, sizes, blend, pulse offset, gains, phase) and calls
  the kernels it picked in `prepare()`. There are two kinds: unison groups side by side, and one
  copy several samples at a time. The SSE2 kernels (`OscillatorSimd.cpp`) are the previous ones
  on the new interface. In the AVX2 kernels (`OscillatorSimdAvx2.cpp`):
  - the groups kernel puts two groups in a register, each lane with its own tables
    (`vpsrlvd`/`vpsllvd`), and sums the lanes into the channels eight samples at a time (an `hadd`
    tree);
  - the single-copy kernel reads eight samples per step, with the phases in closed form
    (p + s v + step s (s + 1) / 2);
  - the groups keep the SSE2 rule for which table a group reads, so both paths sound the same.
  - **What mattered:** MSVC didn't inline the per-sample lambdas: a call per sample, with the
    registers going through memory. Forcing the helpers inline took the groups kernel from 1.42 to
    1.03 ns per copy-sample. Moving two indices per 64-bit transfer gave a few percent more.
  - **Kernel timings** (`lattice_tests "[bench][simd]"`, 16 voices × 8 copies on the benchmark's
    notes, ns per copy-sample):

    | Kernel | SSE2 | AVX2 |
    |---|---|---|
    | Groups | 2.68 | 1.35 |
    | One copy at a time | 5.93 | 1.38 |
  - **Result:**

    | Synth case | Before | AVX2 |
    |---|---|---|
    | 16 voices, unison 1 | 1.8% | 1.1% |
    | 16 voices, ladder | 2.0% (after step 2) | 1.4% (meets the 1.6% target) |
    | 16 voices, unison 4 | 2.8% | 2.15% (target 1.8%) |
  - **Why unison 4 stops short:**
    - with the copies kernel skipped, unison 4 costs 1.06%, so the oscillators are now half of
      it;
    - most copies blend into the next table, which doubles the reads;
    - the rest is per-voice work: envelopes, filters, mixing, and the per-copy table position each
      block (5% of instructions);
    - going further means a different table layout (for example, precomputed interpolation
      coefficients: four times the table memory) or a cheaper blend, both of which change the
      oscillator design. So the unison-4 budget is for step 6.
  - Tests: each kernel against `MipmappedWave`'s own reads at each level (1–4 groups of mixed
    shapes, sizes, blends and pulse widths; 1–64-sample blocks; phases carried across blocks), and
    the Synth rendered at each level for unison 1–8 with saw, pulse, sine and triangle, which must
    match within 1e-5.

## 4. Sampler sinc (2.4% → about 1.5%)

- **Difference rows:** store each phase row with its difference to the next
  (`row0`, `row1 − row0`), so interpolating between phases is one FMA instead of a subtract and an
  FMA: about 15% of the kernel.
- **Batched horizontal sums:** reduce four output samples' accumulators with one transpose instead
  of four separate horizontal sums.
- **Octave copies of each sample** (only if still over budget after the two above): a half-rate
  copy, made when the sample loads, for voices pitched up more than an octave, so their kernels
  stay at 64–128 taps instead of growing to 176+. It costs 50% more sample memory, so it would be
  an option for long samples.
- As built (2026-10-14):
  - **Difference rows:** each phase row is stored with its difference to the next, 64-byte
    aligned (`SincKernelBank`, `SincKernelView`), so a weight is one multiply-add. The table
    is the same size as before.
  - **Four positions per step** (AVX2 in `SincBlockAvx2.cpp`, SSE2 in `SincBlock.cpp`): all
    four read their own phase rows in the same loop (one kernel level per block, so one length),
    and their sums reduce together (an `hadd` tree, or a 4 × 4 transpose with SSE2). A partial
    batch repeats the first position and doesn't store it. One position at a time per step, or
    two with two sets of accumulators, measured slower.
  - **Around the kernel** (`SamplerDevice::renderVoice`): the block's positions are computed in
    closed form (x0 + i·rate + step·i(i + 1)/2, two at a time with SSE2; step by step only when the
    loop's end falls in the block), and the voice mixes into the output four samples at a time.
  - **What the profile showed:** the kernel is limited by memory, not arithmetic. The benchmark's
    32 voices use 12 kernel levels, about 128 KB of rows each, about 1.5 MB in all (L2 is 512 KB),
    and each output sample reads 2 × its taps × 4 bytes of rows. With every voice on one level the
    same kernel runs 40% faster. Software prefetch didn't help (bandwidth, not latency), and
    halving the rows read (a test only) saved 9%.
  - **Kernel timings** (`lattice_tests "[bench][simd]"`, the benchmark's notes, ns per output
    sample):

    | Kernel | Before | After |
    |---|---|---|
    | SSE2 stereo | 36.6 | 24.7 |
    | SSE2 mono | 31.4 | 19.4 |
    | AVX2 stereo | 18.2 | 17.1 |
    | AVX2 mono | 15.5 | 13.8 |
  - **Instructions** (callgrind, the Sampler benchmark): sinc 540M → 434M (−20%), Draft
    216M → 177M (−18%).
  - **Result** (quiet machine, the previous and this build run alternately, 10-second runs, 64 /
    256-sample blocks):

    | Sampler, 32 voices | Before | After |
    |---|---|---|
    | Sinc, AVX2 | 2.44 / 2.27% | 2.25 / 2.07% |
    | Sinc, SSE2 | 4.28 / 4.06% | 3.02 / 2.79% |
    | Draft | 0.99 / 0.95% | 0.90 / 0.86% |

    The 1.5% target is not met. The Sampler's sinc budget is raised to 2.5% (agreed
    2026-10-14); the options below stay open.
  - **Stress test** (10 minutes, reference set, 64-sample blocks, after steps 1–4): no block late;
    load 6% at the median, 8% at the 99th percentile, 48.5% for the worst block.
  - **What's left** (not done; they change memory use or the kernel):
    - smaller rows: the kernel is symmetric (phase f's row is phase 1 − f's reversed), so half the
      rows would do, at the cost of a permute per load for half the positions; or the differences
      in half precision (F16C), 25% fewer bytes per sample with about −100 dB of added error;
    - the octave copies above: the voices pitched up more than an octave read half the taps;
    - or a shorter kernel, which changes the quality spec (−90 dB aliasing, 0.1 dB flatness).
  - Tests: both block kernels against one position at a time (`sincInterpolate`) for every third
    level, stereo and mono, every block length up to 9 and then every 13th up to 61; the Sampler
    rendered at each level must match within 1e-5; the Sampler's quality tests unchanged.

## 5. Reverb (Ultra: 2.6% → about 1.9%; Eco: keep under 0.5%)

- **Eight lines per register** in the tail (32 lines at 2× in Ultra become 4 groups instead of 8).
- **Eco's input stage:** precompute the early taps' positions per block (the tap delays are
  fixed between parameter changes), so the per-sample loop is plain loads and FMAs. The last
  attempt computed slot indices per sample and doubled the instruction count; this one computes
  them in `configure()`.

## Order, checks and stopping rule

1. Infrastructure (1).
2. Ladder (2): the most over budget relative to its share of real projects.
3. Oscillators (3).
4. Sampler (4): difference rows and batched sums; octave copies only if needed.
5. Reverb (5).
6. Re-record callgrind baselines; update "As built" and the budgets; full CI.

After each step: the device's quality tests (aliasing, cutoff accuracy, RT60, latency), the
AVX2-vs-SSE2 comparison, `lattice_bench` on the reference machine (10-second runs, machine quiet),
and the 10-minute reference-set stress test at 64-sample blocks. Each step is its own commit.

If a step misses its target by a lot, its profile decides between going further and asking to
raise that budget.

**Out of scope:** NEON for ARM (macOS on Apple silicon keeps today's scalar paths) and AVX-512
(the reference machine doesn't have it).
