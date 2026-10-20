# Sub-plan M3: Built-in Devices

Parent: `00-preliminary-plan.md`, milestone M3 (component #5). It also picks up return tracks, sends, sidechain, and delay compensation, which M2 left out.
Depends on: M1 (`TrackTuning::hzFor`), and M2 (engine snapshot, the `AudioProcessor` device slot, `RtShared`, `OfflineRenderer`).

## Goal
Every track gets a Live-style **device chain**: MIDI effects → instrument → audio effects. This milestone delivers:
- the core built-in instruments (Synth, Sampler, Drum Rack)
- MIDI effects (Arpeggiator, Chord)
- the core audio effects
- Instrument/Effect Racks with a **configurable number of macros (0–64)**
- return tracks and sends, sidechain, and delay compensation
- **parallel track processing**
- presets

**Priority (see the preliminary plan):** sound quality and performance come before schedule. Each device is built with the highest-quality technique that fits its CPU budget, and the budgets are enforced by tests.

Each device:
- exposes **ordered parameter pages**. Each controller splits them by its own encoder count: 8 on Push and Maschine, 8, 16, or 32 on Virtual Push. Nothing in a device assumes 8.
- has a generic on-screen editor, so everything works without hardware
- uses the track's tuning, so Colundi works in every instrument

**Out of scope here:**
- custom device UIs, which come in M4 (M3 uses auto-generated editors)
- audio tracks and audio clips
- automation (parameter IDs are designed to be stable, ready for it)
- MPE input (the voice design allows for it; turned on in M5 for Push 3)
- pitch detection on samples

**Decided:** samples load fully into memory. Streaming from disk is not planned. This keeps playback simple and fast.

## Phases
3a framework → 3b instruments → 3c audio effects → 3d racks and Drum Rack → 3e returns, sends, sidechain, delay compensation, and parallel processing → 3f presets and editors.
Each phase ends with its tests passing. Phase 3b can start as soon as 3a is done. 3c can run alongside 3b.

## 3a: Device framework — `src/devices/framework/`

### `StudioDevice : juce::AudioProcessor`
- **Parameters:**
  - Each device declares its parameters through a `ParameterSpec` list: a stable string ID, a name, a range and skew, a default value, a unit, a display formatter, and whether it can be modulated.
  - These become `juce::RangedAudioParameter`s and are smoothed on the audio thread.
  - **Parameter IDs never change once released.** Saved sets, presets, macros, Push mappings, and later automation all refer to them.
- **Parameter pages:**
  - Each device declares an ordered list of **parameter groups** (e.g. Synth: "Osc", "Filter", "Amp Env", "Mod"…), with the most important parameters first in each group.
  - A `ParameterPager` turns the groups into pages for one surface's encoder count `N`:
    - A group that fits in `N` stays on one page.
    - Smaller groups are put together on one page where they fit.
    - Larger groups are split into several pages.
    - Page names come from the group names.
  - The same device therefore gets well-arranged pages on an 8-encoder Push and on a 16- or 32-encoder Virtual Push. A device can also declare its own page layout for a given `N` if it wants a specific arrangement.
  - The generic editor shows all groups at once. It does not page.
- **Kinds:** `MidiEffect`, `Instrument`, `AudioEffect`, or `Rack`.
- **State:**
  - Each device's state is a child ValueTree inside the track's `DeviceChain` node in the M2 model.
  - Parameter changes from the UI go through the `UndoManager`. Dragging a control counts as one undo step.
- **Bypass:** a per-device on/off switch. Switching it crossfades over 5 ms, so there are no clicks.
- **Latency:** each device reports it through `getLatencySamples()`. 3e uses this for delay compensation.

### Tuning hookup
- Instruments implement `TuningAware::setTrackTuning(const TrackTuning*)`.
- **Voices keep their pitch as a fractional note** (a note number plus bend, glide, and modulation) and turn it into Hz through `TrackTuning::hzFor` once per block, then smooth it per sample. Because of this:
  - glide and vibrato move smoothly along the tuning table
  - Colundi is respected everywhere
  - per-note bend (MPE, M5) only needs one extra term per voice

### Chain changes (message thread → audio thread)
- Adding, removing, or reordering devices builds a new chain array in the next `EngineSnapshot`.
- New devices run `prepareToPlay`, which may allocate, on the message thread before the snapshot is published.
- Removed devices go to the `RtShared` retire list and are freed after the epoch check.
- Devices that stay in the chain keep their instance, so their sound continues.
- Each track's chain reserves its buffers in advance: its audio buffer, a MIDI scratch buffer, and the dry buffer used for crossfading.

### Shared DSP building blocks — `src/devices/dsp/`
- **Oscillators:**
  - Every shape is a **band-limited wavetable with mipmaps**: saw, pulse, triangle, and the factory wavetables.
  - The tables are built at startup, at 2 tables per octave, using cubic interpolation, with a crossfade between mip levels.
  - The target is aliasing at or below −80 dB.
  - PWM is made from two band-limited saws subtracted from each other.
  - Hard sync uses BLEP correction.
  - The inner loops use SIMD (through `juce::dsp::SIMDRegister` or xsimd), processing 4 or 8 voices or unison copies at once.
- **Filters:**
  - a TPT state-variable filter (LP/HP/BP/notch at 12/24 dB)
  - a nonlinear ladder low-pass, solved with zero-delay feedback and 2× oversampling, so it stays stable and accurate at high resonance
  - biquads for EQ, with coefficients matched to analog filters so they don't warp near Nyquist
- **ADSR** with curve shapes, and an **LFO** (sine, triangle, saw, square, sample-and-hold, synced to tempo).
- **Oversampling** wrapper (`juce::dsp::Oversampling`), plus **parameter smoothing** and **denormal protection** (FTZ/DAZ in the audio callback).
- **Delay lines** (fractional, with interpolation), an **envelope follower**, and a **level detector** (peak/RMS).

## 3b: Instruments and MIDI effects — `src/devices/instruments/`, `src/devices/midi/`

### Synth (like Live's Analog/Drift)
- **Sound sources:**
  - 2 oscillators (saw, pulse with PWM, triangle, sine, wavetable), a sub-oscillator, and noise.
  - Oscillator detune in cents **or in table steps**.
  - Unison of 1–8 voices with detune and stereo spread.
- **Filter:**
  - SVF (LP/HP/BP/notch) or ladder LP.
  - Drive, and **key tracking based on frequency in Hz** rather than note number. This keeps tracking correct in Colundi.
- **Modulation:**
  - 2 ADSRs (amp, plus one assignable), 2 LFOs.
  - An 8-slot mod matrix. Sources: envelopes, LFOs, velocity, aftertouch, mod wheel, key. Destinations: pitch, cutoff, resonance, PWM, level, pan, and so on.
- **Playing:**
  - Polyphony of 1–32 voices.
  - Mono and legato modes.
  - Glide that is measured in fractional notes, so it follows the tuning.
- **Voice stealing:** a released voice goes first; otherwise the oldest; a fast 3 ms fade avoids clicks.
- Velocity sensitivity for amp and filter.

### Sampler (like Live's Simpler)
- **File loading:**
  - Loads WAV, AIFF, FLAC, MP3, and OGG through `juce::AudioFormatManager`, **on a background thread**. Until the sample arrives, the audio thread plays silence, and the sample is swapped in through `RtShared`.
  - Drag-and-drop from the OS file explorer, from the browser list, and onto Drum Rack pads.
- **Modes:**
  - **Classic:** pitched, with an ADSR, a loop, and a crossfade.
  - **One-Shot:** ignores note-off; has fade in/out.
  - **Slice:** by transient, by beat grid, or manually. Each slice maps to a note starting at 36.
- **Pitch:**
  - Each sample stores a **root frequency in Hz** (it defaults to the root note's frequency in 12-TET; the user can edit it).
  - Playback rate = `hzFor(note) / rootHz`. In Colundi, a 440 Hz sample therefore plays at exactly the table frequencies.
  - Transpose works in semitones or in table steps.
- **Interpolation quality:**
  - The default is **windowed-sinc interpolation**: 32 taps, polyphase, with a Kaiser window.
  - When a sample is pitched up, the cutoff scales down so the playback doesn't alias.
  - A "Draft" option uses cubic Hermite interpolation to save CPU.
  - The setting applies per Sampler and per Drum Rack pad.
- **Memory:**
  - Samples are decoded fully into RAM as 32-bit float when loaded.
  - Identical files are shared between devices through a reference-counted sample pool.
- **Other:**
  - Start and end points, a filter (the shared SVF), velocity to volume, and spread.
  - Up to 32 voices, with a choke option.

### MIDI effects
- **Arpeggiator:**
  - Styles: up, down, up/down, played order, random, chord.
  - Rate synced to tempo, gate, steps, and transpose in **table steps**, so it stays within the active tuning.
  - Distance across ranges, and retrigger on new note or beat.
- **Chord:** adds up to 6 notes at given offsets in **table steps**, with a velocity for each. In Colundi this makes chords from table intervals.

## 3c: Audio effects — `src/devices/effects/`

| Device | Main features |
|--------|---------------|
| **EQ Eight** | 8 bands. Each band is a bell, shelf, notch, or LP/HP at 12/24/48 dB. Stereo or mid/side. Sends spectrum-analyzer frames to the UI through a lock-free FIFO |
| **Compressor** | Peak or RMS detection; threshold, ratio, attack, release, knee, makeup, dry/wet; **sidechain** input (see 3e) with a sidechain EQ |
| **Reverb** | Our own high-quality algorithmic reverb. See the "Reverb design" section below |
| **Delay** | Stereo or ping-pong; time synced to tempo or free; feedback with HP/LP filters; modulation; freeze |
| **Auto Filter** | SVF or ladder, with an LFO (synced to tempo) and an envelope follower |
| **Saturator** | Curves: soft, hard, tube, sine fold. Drive, 2× or 4× oversampling, output level, dry/wet |
| **Chorus** | 2–3 delay voices with modulation; rate, depth, feedback, width |
| **Utility** | Gain, pan, width, mono, phase invert per channel, DC filter |
| **Limiter** | 1.5 ms look-ahead with a ceiling. Its latency is reported, so delay compensation covers it (meant mainly for the master track) |

### Reverb design (our own, quality first)
- **Structure:** input diffusion → early reflections → late reverb tail, with separate level controls for early and late.
- **Input diffusion:** 4–6 all-pass stages in series. A diffusion setting controls how much the attack is smeared.
- **Early reflections:**
  - A multi-tap delay whose tap patterns are generated from the room size and shape (presets: room, hall, chamber, plate-like).
  - Taps are panned in stereo, with distance-based damping on each tap.
- **Late tail:**
  - A **16-line feedback delay network** with a Householder or Hadamard feedback matrix. This mixes evenly with no unwanted resonances.
  - Delay lengths are mutually prime and scaled by the size setting.
  - **Each delay length is modulated slightly and at random** (with interpolation) to break up metallic ringing.
- **Decay that depends on frequency:**
  - Each line has a 3-band decay filter (low/mid/high with adjustable crossovers), so the decay time can be set separately for each band.
  - The filter is designed so the measured decay matches the settings.
- **Other controls:**
  - pre-delay (0–500 ms, can sync to tempo)
  - stereo width
  - a high/low cut on the wet signal
  - freeze (infinite hold)
  - dry/wet
  - quality modes: "Eco", "High" (the default), and "Ultra". Ultra adds 2× oversampling of the tail and more delay lines.
- **Engineering:**
  - Fully SIMD: the 16 lines run as 4× or 8× vectors.
  - Denormal-safe.
  - No allocation when parameters change. Delay buffers are sized for the maximum room size in `prepareToPlay`.
  - Changing the size crossfades smoothly, with no zipper noise.

## 3d: Racks and Drum Rack — `src/devices/racks/`

### `Rack` base (used by Instrument Rack, Effect Rack, and Drum Rack)
- **Chains:**
  - A rack holds parallel **chains**, and each chain is a full device chain.
  - Each chain has its own volume, pan, mute, and solo. The rack mixes the chains together.
- **Chain selection** (Instrument Racks) by **key zone** and **velocity zone**, each with fade ranges.
- **Macros:**
  - **The macro count can be set per rack, from 0 to 64** (default 8). It can be changed at any time; removing macros removes their mappings, as an undoable step.
  - Each macro maps to any number of parameters inside the rack, with a min/max range and a curve. Macros can be named and colored.
  - Macro values can be saved as **macro variations** (snapshots that can be recalled), as in Live 11+.
  - The macros are the rack's first parameter group. Each controller pages through them by its own encoder count: 8 per page on Push; 16 or 32 at once on a matching Virtual Push.
- **Limits:** racks can contain racks, up to 4 levels deep. Each chain's buffers are allocated on the message thread when the chain is added.
- **Delay compensation inside a rack:** parallel chains with different latencies are lined up before they are mixed.

### Drum Rack
- **Pads:** 128 pads (note to pad), shown as 16-pad pages. The default page starts at note 36, as in Live.
- **Each pad** holds a chain (by default a Sampler in One-Shot mode) and has its own volume, pan, mute, solo, and **choke group** (16 groups).
- **Pad pitch:** pads trigger by note number, not by tuning, so drums aren't retuned by Colundi. Each pad has its own **tune control in cents or table steps**. A "Tuned pads" option instead sets a pad's pitch from the track tuning.
- **Mouse use:** a 4×4 on-screen pad grid lets you audition pads with the mouse and drop samples onto them. This is the hardware-free counterpart of Push's Drum mode.

## 3e: Return tracks, sends, sidechain, and delay compensation — `src/engine/`
- **Return tracks:**
  - 2 by default (A and B), up to 12.
  - Each has a device chain, volume, pan, mute, and solo.
  - The master track also gets a device chain.
- **Sends:** each track has one send per return, set in dB, and can be pre- or post-fader (a per-return setting, as in Live).
- **Sidechain:**
  - A compressor (and later other devices) can choose another track as its sidechain source, taken either before or after that track's effects.
  - The engine processes tracks in an order where every sidechain source comes first. A sidechain choice that would form a loop is refused in the UI with an explanation.
- **Delay compensation:**
  - Each track's latency is the total latency of its chain, with racks counted along their slowest chain.
  - Tracks with less latency are delayed to match the slowest track before mixing, and the same is done for returns.
  - Meters and clip timing stay correct. Live input monitoring is delayed by the same amount; this is shown in the UI.
  - Delay lines are allocated on the message thread whenever the latency changes.
- **"Reduced latency when monitoring"** (from the performance charter, as in Live): when it's on, armed tracks that are being monitored skip their delay-compensation delay. Playing live then feels immediate even with latency-adding devices elsewhere in the set. The tracks that are only playing back stay compensated.
- **Parallel track processing** (moved here from post-MVP, because performance matters):
  - The routing graph built here already orders tracks and handles sidechain sources, sends, returns, and master.
  - The engine turns that graph into dependency **levels**. Tracks within one level are processed in parallel on a pool of real-time worker threads (one per physical core, minus one).
  - Workers wait in a spin-then-sleep loop, so waking them costs very little.
  - Real-time priority goes through `platform::RealtimeThread`:
    - **Windows:** MMCSS "Pro Audio".
    - **Linux:** `SCHED_FIFO` through rtkit. If real-time priority can't be had, the app warns and points to the system settings that fix it.
    - **macOS:** the audio device's workgroup.
  - The audio-callback thread does work too, so a small project never waits on another thread.
  - Every track writes only to its own buffers. Mixing into returns and master happens in a fixed order once each level finishes, so **the output is identical, sample for sample, to single-threaded processing**.
  - A setting switches multi-threading off for debugging.
- **Snapshot and model additions:**
  - `ReturnTrack[]` and send levels for each track.
  - a sidechain routing table
  - per-track compensation delays
  - the precomputed processing levels

## 3f: Presets, browser list, generic editors — `src/devices/presets/`, `src/app/debug/`
- **Presets:**
  - XML files (`.lpreset`) holding a device's (or a whole rack's) state and its format version.
  - Factory presets are embedded with BinaryData; user presets go in `<userdata>/Library/Presets/<Device>`.
  - Includes several **Colundi starter presets**: a Colundi pad, a Colundi bass, and a Colundi arp chord rack.
- **Device list:** a simple side list of devices, presets, and sample folders. Drag an item, or double-click it, to insert it on the selected track. The full browser is M4.
- **Generic editor** (the debug UI from M2, extended):
  - **Chain row:** the track's device chain as a horizontal row, as in Live. Devices can be added, removed, dragged to reorder, and bypassed.
  - **Device panels:** each device shows all its parameter groups as labeled knob sections, built automatically from the `ParameterSpec`s.
  - **Racks:** the macro panel (it grows with the macro count, with a field to set the count), the chain list, key/velocity zone fields, and macro variations.
  - **Drum Rack:** the 4×4 pad grid.
  - **Mixer:** return tracks, send knobs, and a sidechain source drop-down.

This keeps the rule that every feature works without hardware. M4 replaces the visuals but keeps the same data.

## Colundi in the devices (summary)
| Where | Behavior |
|-------|----------|
| All instruments | Pitch comes from `hzFor(fractional note)`. Glide, vibrato, and bend follow the table |
| Synth | Detune in table steps; filter key tracking by Hz |
| Sampler | Root set in Hz, so playback rate = `hzFor(note) / rootHz` |
| Drum Rack | Pads are untuned by default; "Tuned pads" option; tune control in cents or table steps |
| Arpeggiator / Chord | Transpose and chord offsets in table steps |
| Presets | Colundi starter presets |

## Tests (Catch2) — `tests/devices/`
All tests run in `OfflineRenderer` under `ScopedAudioThread` (from M2) at several block sizes.

1. **Framework:**
   - Every device has unique, stable parameter IDs.
   - `ParameterPager` with N = 4, 8, 16, and 32 puts every parameter on exactly one page, keeps each page within N, and keeps the parameter order.
   - Device state survives save and reload exactly.
   - A preset saves and loads back identically.
   - Bypass crossfades without a click (the largest sample-to-sample step stays below a set limit).
2. **Real-time fuzz:** for every device, change parameters at random, insert, remove, reorder, and bypass while rendering. There must be no allocation on the audio thread, and every output sample must be finite (no NaN or Inf). A separate pass checks that CPU use doesn't jump when the input goes silent (no denormal slowdowns).
3. **Synth:**
   - Colundi notes 12, 47, and 100 measure within 0.5 Hz of the table (using the sine oscillator).
   - Glide between two notes passes through the table's interpolated frequencies.
   - Saw, pulse, and triangle swept from 20 Hz to 15 kHz at 48 kHz have aliasing at or below −80 dB relative to the fundamental.
   - The filter's measured cutoff is within 3% of its setting. The ladder stays stable at maximum resonance, with no runaway output.
   - Key tracking set to 100% doubles the cutoff when the note's Hz doubles.
   - Voice stealing doesn't click.
   - Envelope times are accurate to within 1 ms.
4. **Sampler:**
   - A 440 Hz sine sample with its root set to 440 Hz, played at Colundi note 12, measures 59.9 Hz.
   - Slice mode puts slices on the right notes.
   - One-Shot mode ignores note-off.
   - While a sample loads, the output is silent and nothing is allocated on the audio thread.
   - With sinc interpolation, a sine pitched up by 24 semitones has aliasing at or below −90 dB, and the passband stays flat within 0.1 dB up to 0.9 × Nyquist. Hermite is also measured, as a reference for comparison.
   - Loading the same file into two devices shares one copy in the sample pool.
5. **MIDI effects:**
   - The arpeggiator's note order and timing are exact at every sync rate.
   - A Chord with offsets +3 and +7 steps on note 47 outputs 47, 50, and 54.
6. **Effects:**
   - EQ magnitude response is within 0.1 dB of the analytic biquad.
   - The compressor's static curve matches threshold, ratio, and knee within 0.2 dB.
   - **Reverb:**
     - The measured RT60 for each of the 3 bands is within 10% of its setting.
     - The response to an impulse shows no strong resonance: no spectral peak more than 6 dB above a smoothed envelope in the tail.
     - The left and right channels are decorrelated (correlation below 0.3 at full width).
     - Freeze holds the level steady.
     - Denormals don't build up during a 60 s silent tail.
   - Delay time is exact to the sample at every synced value.
   - The saturator's oversampling measurably reduces aliasing.
   - Utility math is exact.
   - The limiter's output never goes over the ceiling, measured as sample peak.
7. **Racks:**
   - Key and velocity zones send notes to the right chains, with fades.
   - A macro moves its mapped parameters within their ranges and along their curves.
   - The macro count can be set to 0, 8, 16, 32, and 64. Lowering it removes mappings, and undo restores them.
   - Recalling a macro variation restores the exact values.
   - Nested racks render correctly.
   - Chain solo and mute work.
   - Drum Rack choke groups cut off the other pads in the group.
8. **Delay compensation:**
   - An impulse played on tracks with different latencies, and through their sends and returns, arrives at the master on the same sample.
   - The same holds inside a rack whose chains have different latencies.
9. **Sidechain:** a kick on track 1 ducks a compressor on track 2 by the expected amount. A routing that would form a loop is refused.
10. **Parallel processing:**
    - A 32-track project with sends, returns, and sidechains renders **sample-for-sample identical** output with 1, 2, 4, and N worker threads.
    - TSan reports no data races.
    - A stress test runs the realistic 32-track set below at a 64-sample buffer for 10 minutes with zero missed deadlines (measured in the offline harness, with a real-time deadline simulated per block).
11. **Performance (enforced):**
    - **Benchmarks:** a separate `lattice_bench` target (Catch2 BENCHMARK) measures:
      - each device at 48 kHz with 64- and 256-sample buffers
      - Synth (16 voices × 2 oscillators, unison 1 and 4)
      - Sampler (32 voices, sinc)
      - Reverb (High and Ultra)
      - EQ Eight
      - a realistic 32-track set
    - **Starting budgets**, on one core of the reference machine, which is the user's PC:
      - Synth patch: under 2%
      - Sampler: under 1.5%
      - Reverb High: under 1%
      - other effects: under 0.5% each
    - **Baselines** are stored in the repo. **A regression of more than 10% fails** when run on the reference machine. CI runners vary too much in speed, so CI flags a regression of more than 25% as a warning.
    - Budgets are reviewed at the end of M3 and then fixed for later milestones (fixed 2026-10-14:
      the starting budgets, except the Sampler's sinc at 2.5%; see "Performance" below).

## Definition of done
- [x] Load a Synth on a Colundi track, play it from the computer keyboard, and hear correct Colundi pitches, including during glide.
- [x] Build a Drum Rack by dragging samples onto pads, and play it with the mouse and with the computer keyboard.
- [x] Load a 440 Hz sample into the Sampler, set the track to Colundi, and the notes play at the table frequencies.
- [x] Chain Arpeggiator → Synth → EQ → Delay, then reorder and bypass devices while playing, with no clicks or dropouts.
- [x] Build an Instrument Rack with 2 chains and 16 macros, save it as a preset, and load it on another track.
- [x] A sidechain-compressed pad ducks to a kick through return reverbs, and the Limiter on master stays aligned with everything else.
- [x] Every parameter can be reached from the generic editor with no controller connected.
- [x] All device tests pass in CI, and all benchmarks are within budget on the reference machine.
- [x] A 32-track set plays without dropouts at a 64-sample buffer on the reference machine, using parallel processing.

Checked 2026-10-19: the hands-on items on the reference machine (all pass); CI green on every
commit; `lattice_bench` within budget and the 10-minute reference-set stress test clean on a quiet
machine (`docs/perf/avx2-plan.md`, step 6). The hands-on checks found one bug, fixed: a Compressor's
sidechain set up while the set was playing did nothing until the set was reloaded (see
"Returns, sends, sidechain, delay compensation").

## As built (2026-10-12)
Where the code differs from the plan above, and why. Each device's header describes it in full.

### Framework (`src/devices/framework/`)
- **`StudioDevice`** declares its parameters as `ParameterSpec`s (`ParameterSpec.h`); each becomes a
  `DeviceParameter`, a `juce::RangedAudioParameter` holding the document's base value plus an atomic
  override (how rack macros drive parameters without touching the document). A spec's skew centre
  defaults to "none" (a centre of 0 used to skew ranges that span 0).
- **Pages:** `ParameterPager` as planned; racks lay out their own pages (macros first).
- **Document:** a chain is `DEVICES / DEVICE (type, enabled) / PARAMS + STATE`, set format version 3
  (sets from versions 1 and 2 are migrated: every track gets the default instrument, the Synth).
- **Chain changes** go through `SettledShared<T>` (`src/core/`): the engine publishes a track's new
  `DeviceChainLayout` and frees the old one only after the audio thread has switched away from it.
  A chain change fades the track out and in over 3 ms. Parameter, on/off and state changes go
  straight to the live instance, with no rebuild.
- **On/off** crossfades over 5 ms; a latency-adding effect switched off keeps its delay (its dry
  path is delayed by its latency), so nothing shifts. A MIDI effect switched off ends its notes.
- **Unknown device types** (a newer set, a removed device) load as a pass-through `MissingDevice`
  that keeps the saved state, so saving loses nothing.

### DSP building blocks (`src/devices/dsp/`)
- Oscillators: mipmapped band-limited wavetables (2 tables per octave, crossfaded), with SIMD
  kernels: unison groups side by side (4 copies per SSE register, 8 per AVX2 register) and single
  copies 4 or 8 samples at a time (docs/perf/avx2-plan.md, step 3). Filters: TPT SVF, the nonlinear ZDF ladder at 2× through
  half-band filters, matched biquads, and `VoiceFilterSimd` (voice filters side by side: 4 per SSE
  register, 8 per AVX2 register, two registers interleaved; docs/perf/avx2-plan.md, step 2).
  Envelopes render in blocks. Delay lines interpolate with Lagrange; `LevelDetector` for dynamics.
- **Sample interpolation (deviation):** the windowed-sinc kernel has 64 taps at unity, not 32.
  32 taps can't give both −90 dB aliasing and 0.1 dB flatness to 0.9 × Nyquist; 64 do (measured).
  Kernels are stored for 33 bandwidths (1/8 octave apart, down to 4 octaves up), polyphase with
  linear interpolation between phases (each row stored with its difference to the next). The
  block interpolators take four positions per step (docs/perf/avx2-plan.md, step 4).
- **CPU dispatch:** AVX2 + FMA kernels live in `*Avx2.cpp` files, compiled alone with the AVX2 flags
  and outside link-time optimisation, and are chosen at run time (`SincBlock.h`). Only the
  Sampler's sinc path uses one so far.

### Instruments and MIDI effects
- **Synth:** as planned, plus a staged voice renderer (control block 64; four unison copies per
  SSE register; voice filters four at a time). Stolen voices fade out over 3 ms in spare voice
  slots while the new note starts at once.
- **Sampler:** Classic, One-Shot and Slice (transient, beat grid, manual) modes; sinc or Draft
  (Hermite) quality; "Tuning: Fixed" (12-TET from C4) for Drum Rack pads. Samples load in the
  background through the shared `SamplePool` and are padded with silence so the kernels never read
  outside them. A choke stops every voice with a short fade.
- **Arpeggiator and Chord** as planned (offsets and distances in table steps).

### Audio effects
- All nine are built as planned. The Reverb's tail has 8 (Eco), 16 (High) or 32 (Ultra, at 2× the
  sample rate) lines; freeze turns off the line modulation and rounds the line lengths to whole
  samples, so a frozen tail holds without drifting. The tail runs as a kernel over all its lines
  (eight per register with AVX2), and the input stage (pre-delay, early taps, diffusers) a block at
  a time (docs/perf/avx2-plan.md, step 5). The Saturator oversamples with linear-phase FIR
  half-bands.

### Racks (`src/devices/racks/`)
- The rack's structure (`RackLayout`) is part of the track's chain layout and is handed to the
  rack on the audio thread with it. Chain mix values (volume, pan, mute, solo) are live atomics,
  so a fader move needs no rebuild. Macros (0–64) drive parameters through the overrides.
- Variations store the macro values with 17 significant digits, so recalling one is exact.
- **Deviation:** a rack switched off isn't latency-aligned (its chains' compensation applies only
  while it runs).

### Returns, sends, sidechain, delay compensation (`src/engine/`)
- Tracks render in stages: every chain, level by level (sidechain sources before listeners); then
  a serial mix of faders and sends; then the returns; then the master chain.
- **Delay compensation:** tracks line up with the slowest track, returns with the slowest return,
  and the master's input waits for the slowest return. Compensation is capped at 0.5 s. "Reduced
  latency when monitoring" and solo in place work as planned.
- **Deviation:** a sidechain takes the source track's *uncompensated* chain output (before or after
  its effects). The editor offers only sources that can't form a loop
  (`devices/ChainEditing.h`), rather than refusing one with an explanation.
- A chain whose devices stay the same but whose sidechain routes change (a source chosen in the
  editor) switches to the new routes at once, with no fade (found in the hands-on checks).
- **Parallel processing:** `WorkerPool` (one real-time worker per physical core, minus one; Windows
  workers join MMCSS "Pro Audio"). The audio thread and the workers take a level's tracks, then the
  returns, from one atomic ticket that also carries the job's task count, so a late worker can never
  run a task of the next job. Workers spin briefly, then sleep; the audio thread wakes sleepers
  with a futex wake, its only system call (it never waits). Output is bit-identical with 0, 1, 2, 4
  and N workers (tested). "Multi-threaded audio" in the Audio tab switches the workers off.
- **Deviation:** on Linux and macOS the workers ask for real-time priority through
  `juce::Thread::startRealtimeThread` and run at normal priority if that's refused; rtkit, the
  warning, and macOS audio workgroups are not done yet.

### Presets, device list, generic editor (3f)
- **Presets** (`src/devices/presets/`): `.lpreset` XML (a device or a whole rack, with the preset
  and set format versions). Sidechain sources stay behind (they name tracks of another set). User
  presets go in `<userdata>/Library/Presets/<Device>/`. Factory presets (Colundi Pad, Colundi Bass,
  Colundi Arp Chords) are built in `tests/support/FactoryPresets.h`, written to `resources/presets/`
  by `lattice_tests "[.write-factory-presets]"` and embedded; a test fails while they disagree.
- **Chain rules** (`src/devices/ChainEditing.h`): MIDI effects, then one instrument (a new one
  replaces it), then audio effects; audio-only chains on returns, the master and Audio Effect
  Racks; racks nest at most 4 deep and never in themselves.
- **Editor** (`src/app/debug/`, the `lattice_debug_ui` library): a detail view below the session
  grid with the device list (devices, presets, sample folders) and the shown chain's devices.
  Panels are built from the `ParameterSpec`s; right-click a control to type a value or map it to an
  enclosing rack's macro (full range; ranges and curves get their editor in M4). Racks show macros,
  variations, the chain list with zones, and the selected chain; the Drum Rack shows the 4×4 pads
  (clicking a pad arms its track, as Live's selection arm would). The mixer has return strips and a
  send knob per return. The editor is tested headless, and `"[.ui-snapshot]"` renders it to PNGs.

### Tests and tools
- **Deviation:** benchmarks are a custom harness (`tests/bench/DeviceBench.cpp`, `lattice_bench`),
  not Catch2 BENCHMARK, so it can report load at 64- and 256-sample blocks against the budgets.
- `scripts/profile-bench.sh` profiles one benchmark under callgrind in the Linux container.
- The reference set (`tests/perf/ReferenceSet.h`) now uses the M3 devices: EQs, sidechained
  compressors, Reverb and Delay returns and a master Limiter. The engine benchmark takes
  `--workers`; the callgrind baseline was updated for the new set.

### Performance (end of M3; reference laptop, one core, % of real time at 48 kHz)
`lattice_bench` (Release, Windows, the reference laptop); the first figure is at 64-sample blocks,
the second at 256. Each device runs on one core. "Before" is the end of the M3 performance pass;
"after" is the AVX2 plan (`docs/perf/avx2-plan.md`), the median of three 10-second runs on a quiet
machine (2026-10-14).

| Device | Before | After the AVX2 plan | Budget |
|--------|--------|---------------------|--------|
| Synth, 16 voices × 2 osc, unison 1 | 1.8 / 1.75 | 1.0 / 0.99 | 2 |
| Synth, 16 voices × 2 osc, unison 4 | 2.7 / 2.7 | 1.94 / 1.92 | 2 |
| Synth, 16 voices × 2 osc, unison 4, no spread | | 1.84 / 1.83 | 2 |
| Synth, 16 voices × 1 osc, 12 dB | | 0.89 / 0.85 | 2 |
| Synth, 16 voices, ladder | 2.55 / 2.5 | 1.26 / 1.25 | 2 |
| Sampler, 32 voices, sinc | 2.5 / 2.3 | 2.24 / 2.06 | 2.5 (raised from 1.5, agreed 2026-10-14) |
| Sampler, 32 voices, Draft | 1.0 / 1.0 | 0.89 / 0.86 | 1.5 |
| Reverb, High | 0.8 / 0.75 | 0.34 / 0.32 | 1 |
| Reverb, Ultra | 2.7 / 2.5 | 1.0 / 0.96 | 2 |
| Reverb, Eco | 0.54 / 0.49 | 0.24 / 0.22 | 0.5 |
| Saturator 4× | 0.27 / 0.26 | 0.26 / 0.24 | 0.5 |
| EQ Eight, Compressor, Delay, Chorus, Auto Filter, Saturator 2×, Limiter, Utility | 0.003 – 0.33 | 0.001 – 0.31 | 0.5 |

Every case is within its budget, and the budgets are fixed at these values. The Synth's unison 4 is
the closest (3% under); below the budgets, the 10% benchmark gate and the 5% instruction-count gate
(charter §3) catch regressions.

What the performance pass did: the Sampler's sinc kernel in AVX2 + FMA over blocks of positions
(6.4 → 2.4%) and its Draft path four positions at a time (1.9 → 1.0%); the Reverb's tail four lines
per SSE register (High 1.06 → 0.8%); the half-band filters and the Saturator in blocks (4×: 0.95 →
0.27%), with Ultra changing rate in blocks too (3.9 → 2.6%).

Why the Sampler's sinc budget is 2.5: the kernel's 64 taps are what the −90 dB / 0.1 dB quality
spec needs (a Kaiser design for that transition band needs about 57), and the kernel's multiply-adds
are most of the cost even after step 4 of the AVX2 plan.

**The reference set** (32 tracks, `[stress][timing]` test): 10 minutes at 64-sample blocks, one block
per period of the real clock on a real-time (MMCSS Pro Audio) thread with 7 workers: no block late;
load 8% of the period at the median, 10% at the 99th percentile, 66% for the worst block. After the
AVX2 plan (quiet machine, 2026-10-14): no block late; 5.5% at the median, 7% at the 99th percentile,
8.5% at the 99.9th, 17% (0.23 ms) for the worst block. Its callgrind count went from 3.82G to 2.97G
instructions (−22%), the new baseline.

## Interfaces kept for later milestones
- **M4 UI:** custom device panels replace the generic editor and read the same `ParameterSpec`s and parameter groups. The EQ spectrum FIFO and the Drum Rack pad grid get finished visuals.
- **M5 Push and Virtual Push:**
  - Device mode asks `ParameterPager` for pages that match the surface's encoder count (macros first for racks).
  - Drum mode drives the Drum Rack pages. The page size follows the surface's grid: 16 pads on Push's 4×4 drum area, more on a larger Virtual Push grid.
  - Push 3 MPE adds per-note bend to the voice pitch.
- **M6 plugins:** hosted VST3/CLAP plugins become `StudioDevice` wrappers, with parameter groups generated automatically and paged by `ParameterPager`. They report latency into the same delay compensation, and use racks, sends, and sidechain just like built-in devices.
- **Automation (post-MVP):** works through the stable parameter IDs and the smoothing that is already in place.
