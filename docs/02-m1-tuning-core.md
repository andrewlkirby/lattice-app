# Sub-plan M1: Tuning Core (Colundi)

Parent: `00-preliminary-plan.md`, milestone M1 and component #4 (first part).
Depends on: M0 (`lattice_core` target, test runner, RealtimeCheck).

## Goal
A tuning service that every sound-making part of the studio asks "what frequency is MIDI note *n* on this track?". It comes with:
- the Colundi table as a built-in tuning
- 12-TET as the default tuning
- a test synth, so Colundi can be heard and played from MIDI input or the on-screen keyboard

This is the first audible Colundi result.

Out of scope here (later milestones):
- MTS-ESP master
- MPE / pitch-bend output to hardware
- `.tun` export (all M6)
- the Push pad layout (M5)

The API is designed so those plug in without changes.

## Source data: `colundi_scale.txt`
- Tab-separated. Each data row **starts with a tab**, so the first field is empty.
- Columns: `#`, `Freq`, `Cents`, `Ratio`, `Name`. The header line starts with `#`.
- **128 rows, indices 0–127.** The `Freq` column is the source of truth (e.g. `59.900Hz`), with the `Hz` suffix stripped.
- Columns we don't rely on:
  - `Cents` turns into scientific notation (`1.013e+4`) from row 101, so it loses precision. It is not used for pitch.
  - `Ratio` is rounded to 3 decimals and is not used for pitch.
  - `Name` uses comma decimals (`1,06666`). It is stored as a display label only.
- Facts the tests rely on:
  - Base 30 Hz at index 0
  - index 12 = 59.9 Hz (not 60, so the table does not repeat by octave)
  - index 47 = 440 Hz
  - index 119 = 28160 Hz
  - index 127 = 44098.56 Hz
  - frequencies strictly increase

## Design

### `TuningTable` (immutable value type) — `src/tuning/TuningTable.h`
```cpp
struct TuningTable {
    juce::String id;          // "builtin.12tet", "builtin.colundi", "user.<file-hash>"
    juce::String name;        // "Colundi", "12-TET A440"
    std::array<double, 128> hz;
    std::array<juce::String, 128> labels;   // e.g. "C4", or Colundi index + Hz
    juce::String sourceDescription;
    double frequencyFor(double fractionalNote) const noexcept; // log-interpolated
};
```
- `frequencyFor(n + x)` returns `hz[n] * pow(hz[n+1] / hz[n], x)`, clamped at the table edges. This is what lets pitch bend and glide work on a table that has no fixed step size.
- The type is never changed after construction. It is shared as `std::shared_ptr<const TuningTable>`.

### Parsers — `src/tuning/parsers/`
- **`ColundiTextParser`**:
  - Reads the tab-separated format above.
  - Reports errors with line numbers: wrong row count, a row that isn't a number, frequencies that don't increase, a missing `Hz` field.
  - Number parsing does not depend on the system's locale settings. It uses `std::from_chars` or the `juce::CharacterFunctions` equivalent, never `atof`. The European comma style in the Name column must not break anything on any machine.
  - Builds labels like `"#47 · 440.00 Hz"`.
- **`ScalaParser`** (`.scl` + optional `.kbm`):
  - The standard Scala format: cents or ratio lines and `!` comments.
  - Without a `.kbm` file, it uses the default mapping (note 60 = 261.6256 Hz, linear mapping).
  - Comes in M1 so that user tunings are supported from the start and the parser can be tested against known `.scl` files.
- **`Tet12`**: a generator for the default tuning (A4 = 440 Hz, adjustable reference).

### `TuningRegistry` — `src/tuning/TuningRegistry.{h,cpp}`
- Runs on the message thread only. It holds built-in tunings plus tunings loaded from a user folder (`<userdata>/Tunings`; `<userdata>` is the user data folder for each platform, defined in the preliminary plan).
- Colundi is **embedded through JUCE BinaryData** from `resources/tunings/colundi_scale.txt`, so the app works without that file on disk.
- API:
  - `getAll()`
  - `findById(id)`
  - `loadFile(path) -> Result<shared_ptr<const TuningTable>>`
  - `addListener()`: the hook that M6's MTS-ESP master will use

### `TrackTuning` (per-track state, safe on the audio thread) — `src/tuning/TrackTuning.{h,cpp}`
- Settings:
  - the chosen tuning id
  - `noteOffset` (int, shifts the whole table up or down by an index count)
  - `bendMode` (`Cents` or `TableSteps`)
  - `bendRange` (default ±200 cents, or ±2 steps)
- **Handing tunings to the audio thread:**
  - The message thread publishes the current `const TuningTable*` with an atomic pointer swap.
  - The registry keeps the old `shared_ptr` alive in a "retired" list, which is cleared on a timer after a grace period.
  - Result: the audio thread never takes a lock, allocates, or frees a tuning. This follows the RealtimeCheck rules from M0.
- **Lookup on the audio thread:** `double hzFor(int note, float bend) const noexcept`. It applies the offset, then the bend in the chosen mode, then the high-frequency rule below.
- **Notes above the playable range:**
  - Default policy `Mute`: voices at or above 0.45 × sample rate don't start. At 48 kHz the cutoff is 21.6 kHz, which affects Colundi rows 115–127.
  - Other policy: `FoldDown`, which divides by the scale's period (938.667 for Colundi, 2.0 for octave scales) until the note is in range.
  - The policy is a per-track setting.

### Test synth (temporary until M3) — `src/devices/test/TuningTestSynth.{h,cpp}`
- A `juce::Synthesiser` with 16 voices. Each voice is a sine plus a soft saw with a simple ADSR.
- The voice pitch comes from `TrackTuning::hzFor` instead of `MidiMessage::getMidiNoteInHertz`. This is how every M3 instrument will get its pitch.
- Gets MIDI from `juce::MidiMessageCollector`, fed by the system MIDI inputs, the on-screen keyboard, and the computer keyboard.

### Computer MIDI keyboard — `src/control/ComputerKeyboardInput.{h,cpp}`
Lets you play notes with no MIDI hardware, using the same keys as Live:
- **Note keys:** `A W S E D F T G Y H U J K O L P ;` play 17 notes in a row, from the lowest to the highest. In 12-TET this is the usual piano layout: the A-S-D row is the white keys and the W-E-T row is the black keys. Because the keys already map to notes in a row, **Colundi needs no special layout**: the same 17 keys play 17 consecutive table entries, and the readout shows each entry's index and Hz.
- **Shifting the range:**
  - `Z` / `X` shift the range down or up.
  - The step size is 12 in 12-TET. It is set per tuning, and the default for Colundi is 17, so each press moves to the next page of 17 table entries.
  - The range is clamped so it stays within 0–127.
- **Velocity:** `C` / `V` lower or raise the velocity in steps of 20, from 1 to 127 (default 100).
- **On/off:** a toggle (`M`, as in Live) and a button in the UI. The keyboard is off while a text field has focus.
- **Mechanics:**
  - A `juce::KeyListener` on the main window, plus `keyStateChanged` with `isKeyCurrentlyDown` to catch key releases.
  - Auto-repeat while a key is held is ignored, so a held key doesn't retrigger.
  - All held notes are released when the keyboard is turned off, the window loses focus, or the range shifts.
  - Feeds the same `MidiMessageCollector` as hardware MIDI. In M2 it sends to armed tracks the same way a hardware keyboard does.
- **Structure:** the key-to-note mapping is a pure function, `noteForKey(key, baseNote) -> optional<int>`, so tests can check it without a UI.

### UI additions (in `MainWindow`, replaced by the real UI in M4)
- A drop-down for choosing the tuning.
- A `juce::MidiKeyboardComponent` whose key labels show the table index and Hz when a non-12-TET tuning is active.
- A "last note" readout (index, Hz, label).
- A MIDI input selector.
- A computer keyboard on/off button, with the current range (first and last index, or note name) and velocity shown next to it.
- A "Load tuning file…" button that accepts `.scl` and Colundi `.txt` files.

## Tests (Catch2) — `tests/tuning/`
1. **Colundi parse:**
   - All 128 rows parse.
   - Every `hz[i]` matches the file's Freq value within 0.001 Hz. The test reads the file itself and compares row by row.
   - Spot checks for indices 0, 12, 47, 119, and 127.
2. **Edge rows:** rows 101–127 (cents in scientific notation) and every comma-decimal Name parse without error.
3. **Bad input:** the parser returns the right error and line number for:
   - 127 rows instead of 128
   - a row that isn't a number
   - a non-increasing frequency
   - an empty file
4. **Locale:** set the C locale to one that uses comma decimals (e.g. `de_DE`) when available and parse again. The results must be identical.
5. **Interpolation:**
   - `frequencyFor(47.0) == 440`.
   - `frequencyFor(47.5)` is the geometric mean of rows 47 and 48.
   - Values clamp at 0 and 127.
6. **Bend modes:**
   - +200 cents on 440 Hz gives 493.88 Hz.
   - +1 table step on note 47 gives `hz[48]`.
7. **Scala:** a known 12-TET `.scl` matches `Tet12` within 1e-6. A just-intonation `.scl` matches its hand-computed ratios.
8. **High-frequency policy:**
   - At 48 kHz, note 127 is muted under `Mute`.
   - Under `FoldDown`, note 127 is brought back below Nyquist.
9. **Real-time swap:** a stress test with a writer thread swapping tunings while a reader thread calls `hzFor` in a loop. It must have no crashes or data races (also run with TSan on macOS CI), and nothing is freed on the reader thread.
10. **Computer keyboard mapping:**
    - With the default range in 12-TET, `A` gives the range's first note and `;` gives first + 16.
    - In Colundi, the 17 keys give 17 consecutive indices.
    - `Z`/`X` move by 12 in 12-TET and by 17 in Colundi, and the range stays within 0–127.
    - `C`/`V` velocity stays within 1–127.
    - Auto-repeat doesn't retrigger a held note.
    - Turning the keyboard off, losing focus, or shifting the range releases all held notes.
11. **Offline render:**
    - Render notes 0, 12, 47, and 100 through `TuningTestSynth` with no audio device.
    - Goertzel or FFT with parabolic interpolation confirms each fundamental within 0.5 Hz of the table value. (Note 100 is 9480 Hz, which is still well below Nyquist.)

## Definition of done
- [x] Choosing "Colundi" and playing the on-screen keyboard or a MIDI keyboard produces the table frequencies, with the readout showing the index and Hz. (Checked by the user on 2026-10-11: "it sounds good so far".)
- [x] With no MIDI hardware connected, the computer keyboard can play every Colundi entry that is in range, by shifting through the 17-note pages. (Checked by the user on 2026-10-11: "it sounds good so far".)
- [x] Switching tuning while notes are held causes no glitches or crashes, and the RealtimeCheck stays quiet. (Checked by the user on 2026-10-11: "it sounds good so far".)
- [x] A user `.scl` file loads and plays. (Checked by the user on 2026-10-11: "it sounds good so far".)
- [ ] All tuning tests pass in CI. (Pass locally on Windows and in Docker on 2026-10-11, including TSan; CI runs on push.)

## As built (2026-10-11)
Where the code differs from the plan above, and why:
- **No `juce::Synthesiser` and no `juce::MidiMessageCollector`.** Both take a lock on the audio
  thread, and the collector can allocate there. That breaks the charter's rules, and RTSan would flag
  it. Replacements:
  - `TuningTestSynth` has its own 16-voice allocator (oldest voice stolen), applies MIDI at sample
    offsets, and renders a sine plus a PolyBLEP saw softened by a one-pole low-pass, with the saw
    faded out near Nyquist.
  - `MidiInputQueue` (`src/engine/`): producers (MIDI input thread, on-screen and computer
    keyboards) serialise among themselves with a mutex that only non-real-time threads take. The
    audio thread is the single, lock-free consumer of an `AbstractFifo`. Live input plays at the
    start of the next block (lowest latency; jitter up to one buffer). Short messages only.
- **`RtShared<T>` (`src/core/RtShared.h`) was built now instead of in M2**, and `TrackTuning` uses
  it. It follows the M2 design without timers:
  - Readers open a scope (two atomic increments), and retired objects are freed by `collect()` once
    the open-scope count is zero after their retirement.
  - It's safe with several reader threads (the M3 parallel engine).
  - The play panel calls `collectGarbage()` at 15 Hz.
- **Number parsing:** a strict format check, then `juce::CharacterFunctions::readDoubleValue`, which
  converts with `strtod_l` in the "C" locale. A comma decimal in a Freq field is an error, never
  misread. The locale test runs under `de_DE.UTF-8` on Windows and Linux, and proves the locale is
  active (the C library reads "1.5" as 1.0).
- **Colundi's period ratio is 28160/30 (938.667).** Entry 119 is exactly 30 Hz × 938.667, and the
  Name column wraps there. Only `FoldDown` uses it. A generic Colundi-format `.txt` gets 2.0 unless
  the caller passes one.
- **Scala:**
  - Blank lines after the description line are ignored (most files end with one).
  - `.kbm` files are supported in full (map size, first/last note, middle note, reference, formal
    octave, `x` entries). A `.kbm` with the same name next to a `.scl` is picked up automatically.
  - Unmapped notes have frequency 0 and don't sound.
- **Labels:** 12-TET uses note names with middle C (note 60) as C4. Other tables use
  `#47 · 440.00 Hz`. The on-screen keyboard shows the table index on white keys for non-12-TET
  tunings, and the last-note readout gives index and Hz.
- **Computer keyboard pages** are multiples of the page size from 0, so for Colundi: 0, 17, …, 119.
  - Keys past 127 on the last page play nothing.
  - The default page is the one holding the note nearest middle C: 60 in 12-TET, 34 in Colundi (34–50, which includes #47 = 440 Hz).
  - Computer-keyboard notes go through the on-screen keyboard's state, so its keys light up.
- **User tunings:** files loaded with "Load tuning file…" are remembered (settings
  `userTuningFiles`) and reloaded at startup. Everything in `<userdata>/Tunings` is loaded too. The
  chosen tuning and the keyboard on/off state are saved.
- **ThreadSanitizer** (`linux-tsan` preset, `scripts/linux-test.sh --tsan`, and a CI job) runs the
  whole suite, including the `[stress]` swap test.
  - TSan's runtime defines its own `operator new`/`delete`, so that build doesn't link Lattice's
    allocation hooks. The four allocation-counting tests skip there, and every other build runs them.
  - TSan's container needs `--security-opt seccomp=unconfined` so TSan can restart with address
    randomisation off. Only the TSan run sets it.
- **Tests:** 68 Catch2 cases in the default run (69 with the heavy tags). They pass on Windows
  (Debug, Release, RelWithDebInfo + RT checks) and on Linux (GCC, RTSan, TSan, clang-tidy).

## Interfaces kept for later milestones
- **M3 devices:** every instrument voice uses `TrackTuning::hzFor`.
- **M5 Virtual Push and Push hardware:** the Colundi pad layout reads `TuningTable` labels and frequencies. The "anchor note" colors use `hz[i] / 30.0` being a whole number.
- **M6 external:** the MTS-ESP master listens to `TuningRegistry`. `.scl`/`.kbm`/`.tun` exporters sit next to the parsers. The MPE retuner calls `frequencyFor`.
