# Sub-plan M6: Plugin Hosting, External Hardware, and Retuning Outside the App

Parent: `00-preliminary-plan.md`, milestone M6. Covers component #6 (plugin hosting), #9 (MIDI I/O and external hardware), and the rest of #4 (tuning outside the app).
Depends on:
- M1: tuning tables and parsers
- M2: engine and transport
- M3: `StudioDevice`, `ParameterPager`, delay compensation, sidechain, parallel processing
- M4: browser and library, Preferences, tuning editor
- M5: per-note expression routing, the Surface Monitor pattern

## Goal
1. **Host third-party plugins** (VST3, CLAP, and AU on macOS) as normal devices. This covers:
   - safe scanning in a separate process
   - optional crash isolation
   - parameter pages for controllers
   - correct delay compensation
   - sidechain and multi-output support
2. **Retune plugins to Colundi**, three ways:
   - **MTS-ESP** (we act as the tuning master)
   - tuning **file export** (`.scl`/`.kbm`, `.tun`)
   - **MIDI Tuning Standard** sysex
3. **Play and retune external MIDI hardware:**
   - **MPE / per-note pitch bend**, MTS sysex, or mono bend
   - an **External Instrument** device (MIDI out plus audio return, with latency compensation)
   - **MIDI clock** in and out
   - a **Tuning Verifier** that measures whether your hardware is really playing in tune

**Priority (from the preliminary plan):** quality and performance come first. That means:
- pitch accuracy within 0.1 cent wherever the protocol allows it
- MIDI timing jitter under 1 ms
- no extra latency from hosting unless the user chooses isolation

**Out of scope here:**
- VST2: its SDK can no longer be licensed
- LV2 on Windows and macOS
- Ableton Link (post-MVP; check its license first)
- CV output through a DC-coupled interface (post-MVP)
- routing MIDI from one track's plugin to another track (post-MVP)
- recording MPE into clips (post-MVP, as stated in M5)

## Phases
6a scanning and the plugin library → 6b hosting as devices → 6c crash isolation → 6d tuning for plugins (MTS-ESP and exports) → 6e MIDI out, retuning hardware, External Instrument → 6f clock sync and the Tuning Verifier.
6d can be built alongside 6b and 6c.

## 6a: Scanning and the plugin library — `src/hosting/scan/`
- **Formats:**
  - **VST3** through JUCE.
  - **AU** on macOS through JUCE.
  - **LV2 on Linux** through JUCE's LV2 hosting. It's optional within M6, but it's cheap and matters on Linux, where many plugins only exist as LV2.
  - **Linux plugin folders:** `~/.vst3`, `/usr/lib/vst3`, `/usr/local/lib/vst3`, `~/.clap`, `/usr/lib/clap`, plus the LV2 paths (`~/.lv2`, `/usr/lib/lv2`).
  - **CLAP:** check at the start of M6 whether the JUCE version we use can host CLAP. If not, we write our own CLAP host from the official CLAP headers (MIT license) behind the same `PluginFormat` interface. CLAP gets first-class support because it handles per-note expression and threading well.
- **Scanning in a separate program** (`LatticePluginScanner.exe`):
  - The scanner is run through JUCE's child-process tools. Each plugin gets a **time limit** (default 30 s), and a crash or hang is caught.
  - Plugins that fail are placed on a **blocklist**, with the reason. You can retry them from Preferences.
  - Scans are **incremental**: a plugin is rescanned only when its file path, size, or modification time changed. Scanning runs in the background while you work.
  - Results go into the **M4 SQLite library** (name, vendor, format, category, inputs/outputs, MIDI in/out, MPE or note-expression support, the plugin ID and version).
- **Browser:**
  - A "Plugins" category in the M4 browser, grouped by vendor, format, or category, with search.
  - Favorites and collections work for plugins too.
  - Dragging a plugin in works like a built-in device.
- **Preferences → Plugins:** folders, rescan, the blocklist, the default isolation mode (6c), and the preferred format when a plugin exists in several formats.

## 6b: Hosting plugins as devices — `src/hosting/`

### `HostedPluginDevice : StudioDevice`
- Wraps a `juce::AudioPluginInstance` (or our CLAP instance), so it slots into chains, racks, Drum Rack pads, returns, and master like any built-in device.
- **Parameters → `ParameterSpec`s:**
  - The plugin's own parameter groups (VST3 units, CLAP modules) become **parameter groups**. `ParameterPager` pages them for any encoder count (M3/M5).
  - **Configure mode** (as in Live): touch parameters in the plugin's window to choose and order the ones you want on your devices.
  - The choice is saved in the set, and can also be saved as the **default for that plugin**, so new instances start with the same pages.
  - Macros (any count) can map to plugin parameters.
- **State:**
  - The plugin's state blob is stored in the set as base64, together with the plugin ID and version.
  - **If a plugin is missing:** a placeholder device keeps the blob and the routing, so the set loads fine and nothing is lost. The plugin is restored when it's installed again.
  - Presets: our `.lpreset` wraps the plugin state. `.vstpreset` files can be imported and exported.
- **Latency:**
  - Read from the plugin when it is prepared, and updated whenever the plugin reports a change.
  - A change recalculates M3's delay compensation on the message thread. The new delays are swapped in with a short crossfade on the affected tracks, so playback continues with no dropout.
- **Sidechain:** a VST3 or CLAP plugin's sidechain input appears in the M3 sidechain source picker.
- **Multi-output instruments:** extra output pairs can be sent to separate mixer channels, which appear in the mixer like Live's multi-out routing. They are part of the routing graph, so delay compensation and parallel processing include them.
- **MIDI out from plugins:** passed to the next device in the same chain (e.g. an arpeggiator plugin feeding a synth).
- **Per-note expression:** M5's per-note pitch, timbre, and pressure are sent in each plugin's best format:
  - CLAP note expressions
  - then VST3 Note Expression
  - then MPE MIDI, for plugins that say they support MPE
  - otherwise channel messages only
- **Real-time rules:**
  - Our RealtimeCheck stops asserting while code inside a plugin runs, since we can't fix plugin code. Instead it **counts allocations and lock waits made by each plugin**.
  - Those counts appear in a plugin diagnostics panel, so a misbehaving plugin can be spotted.
  - Plugin processing runs on M3's parallel worker threads.
- **CPU:** each device shows its CPU use, as in Live 12, measured as time per block. Every device gets this, built-in ones included.
- **Plugin windows:**
  - Floating windows with an optional always-on-top setting. The position and size are remembered per device.
  - On Windows, each window follows its monitor's DPI scale (VST3 content scale / CLAP GUI scale). JUCE's DPI awareness setting is checked against several plugins known to have DPI problems.
  - Windows open and close quickly, and a closed window frees its GUI resources.
- **Changing sample rate or buffer size:** devices are re-prepared on the message thread, as with built-in devices, without rebuilding the set.

## 6c: Crash isolation (sandboxing) — `src/hosting/sandbox/`
Running a plugin in the app's own process is fastest, but a crash in the plugin takes down the whole app. So isolation is **user-selectable**, as in Bitwig.
- **Modes** (set as a default in Preferences, and per device):
  - **In-process:** the default, and the fastest.
  - **Shared sandbox:** all isolated plugins run in one helper process.
  - **Per vendor:** one helper process per plugin maker.
  - **Per plugin:** one helper process per plugin.
- **How audio crosses between processes:**
  - Audio and MIDI pass through a **shared-memory ring buffer**, and the two processes wake each other with OS events.
  - **Same-block mode (default):** the plugin's output comes back in the same audio block, so there's no added latency. The cost is extra CPU for the handoff.
  - **Pipelined mode:** adds one block of latency, which is reported to delay compensation. Use it for heavy plugins or when CPU is under pressure.
  - The helper's audio thread runs at real-time priority through `platform::RealtimeThread` (MMCSS on Windows, rtkit `SCHED_FIFO` on Linux, the audio workgroup on macOS).
  - **Cross-process signaling** goes through `platform::IpcSignal`: named events on Windows, futex or POSIX semaphores on Linux, with shared memory from `shm_open`.
  - The plugin's window still opens from the helper process, as a normal window.
- **If a plugin crashes:**
  - Only that device goes silent and shows an error badge. The app and every other track keep playing.
  - You can reload the device with its last saved state.
  - The crash is logged with the plugin's name and version, and the plugin can be added to the blocklist.
- **Cost:** the extra CPU per isolated plugin, and per process, is benchmarked in `lattice_bench`. It's shown in the UI so you can choose a mode knowing the cost.

## 6d: Tuning for plugins — `src/tuning/external/`

### MTS-ESP master
- Uses ODDSound's MTS-ESP SDK as the **tuning master**. Check the SDK's license terms before release.
- **Setup:** MTS-ESP needs ODDSound's shared library installed on the system (it comes with the free MTS-ESP Mini). If it's missing, the app shows how to install it and falls back to file exports.
- **Linux:** check at the start of M6 whether ODDSound offers the library for Linux. If not, Linux uses the CLAP tuning extension (where available), file exports, and MTS sysex.
- **Only one master is allowed system-wide.** If another master is already running, we report that clearly and leave it alone.
- **Sending tunings:**
  - **One tuning in use:** the master table is set to that tuning's 128 Hz values.
  - **Several different tunings in use:** **multi-channel mode** is turned on. Each tuning gets one of 16 MIDI channels, and MIDI going to an MTS-ESP plugin is put on the channel of its track's tuning.
    - A maximum of 16 different tunings can be active at once; the app warns if you go over.
    - The MPE expression path is then disabled for that plugin, since MPE also needs the channels.
  - **Notes outside the playable range** (M1 policy) are **filtered**, so MTS-ESP-aware plugins skip those notes.
  - Tuning changes (from the tuning editor, a track's tuning setting, or the note offset) are sent at once from the message thread.
- **Tuning name:** the master reports the tuning's name ("Colundi") so client plugins can display it.

### CLAP tuning
- If the CLAP tuning extension is stable in the SDK version we use, we provide each track's table to CLAP plugins directly, per track. That avoids the 16-channel limit.

### File and sysex export (also usable from the M4 tuning editor)
- **`.tun` (AnaMark v2):** 128 exact frequencies, one per note. This is the most faithful export for Colundi.
- **`.scl` + `.kbm`:**
  - Colundi doesn't repeat by octave, so it's exported as a **127-step scale whose period is `hz[127]/hz[0]`**.
  - A `.kbm` mapping goes with it: one key per step, starting at note 0, with the reference frequency 30 Hz on note 0.
  - Reading the files back with M1's parser must reproduce the same 128 frequencies.
  - A "12-note approximation" option is **not** offered, because it would quietly change the scale.
- **MIDI Tuning Standard sysex:**
  - **Bulk tuning dump** (all 128 notes).
  - **Single-note tuning change** (real-time), so a change can be sent while notes are held.
  - Each frequency is encoded as a semitone plus a 14-bit fraction: about 0.006 cent resolution.
  - Can be saved as a `.syx` file or sent straight to a hardware port (6e).

## 6e: MIDI out, retuning hardware, External Instrument — `src/engine/midiout/`, `src/devices/external/`

### Sending MIDI to hardware with accurate timing
- **Routing:** a MIDI track without an instrument can send to any MIDI output port and channel (the M4 mixer I/O already shows this).
- **Scheduling:**
  - Events are timestamped exactly to the sample in the engine, converted to the system's MIDI clock, and shifted by a **per-port latency offset** in ms.
  - **Windows MIDI Services** (when available) schedule messages by timestamp.
  - **Linux:** the **ALSA sequencer queue** schedules events by timestamp, which suits the jitter target well.
  - Otherwise, a dedicated high-priority MIDI output thread sends them, using high-resolution waits.
  - **Target:** jitter at or below 1 ms (p99), checked by a test.

### Retuning external hardware (chosen per device in a **Hardware Profile**)
1. **MPE / one channel per note:**
   - Each note is sent on its own channel: an MPE lower zone, or a range of channels you choose for synths that aren't MPE.
   - **Pitch bend for each note:** the base note is the closest 12-TET note. The bend is `(target cents − base note cents) / bend range`, at 14-bit resolution.
   - Bend ranges: 14-bit bend gives 16,384 steps across the whole range. So ±2 semitones (400 cents) gives about 0.024 cent steps, and ±48 (9,600 cents, the MPE default) gives about 0.59 cent steps. The profile's range **must match the synth's setting**. The wizard below helps check it.
   - **Sent before the note-on:** the bend goes out before the note starts, so there's no pitch slide at the attack.
   - **Choosing a channel:** prefer free channels, then the one that was released longest ago, so release tails keep their pitch.
   - **When channels run out:** the oldest note is stolen, with a note-off sent first.
   - Per-note expression from M5 (slides) is added on top of the retuning bend.
2. **MTS sysex:** for synths that support the MIDI Tuning Standard.
   - A bulk dump of the track's tuning is sent when the device connects and whenever the tuning changes. Changes made while notes are held use single-note real-time messages.
   - This keeps the synth's **full polyphony on a single channel**.
3. **Mono pitch bend:** for monophonic synths. A single channel, with the bend set for each note, and glide sent as a bend sweep.
4. **No retuning:** plain notes, with a warning badge showing that this hardware won't play the tuning.

- **Profile wizard:**
  1. Choose the method.
  2. Play test notes.
  3. Optionally run the Tuning Verifier (6f), which **checks the bend range automatically**: it plays a known bend and measures the result.

  Profiles can be saved and reused, and they're shared through the library.

### External Instrument device
- One device that combines:
  - MIDI out (port, channel, and the retuning profile)
  - an **audio return** from the audio interface's inputs
  - a **hardware latency** setting in ms, which can also be **measured automatically** (see the Tuning Verifier)
- **Engine addition:** live audio input is passed through the track, with monitoring and delay compensation. This is also the base for audio tracks after the MVP.
- **External Audio Effect device:** sends audio to interface outputs and returns from inputs, with latency measurement, for outboard effects.

## 6f: Clock sync and the Tuning Verifier

### MIDI clock
- **Clock out:**
  - 24 PPQN clock, start, stop, continue, and song position pointer.
  - A latency offset per port.
  - Ticks are scheduled exactly to the sample through the timed output path above.
- **Clock in (follow an external tempo):**
  - A **phase-locked loop** smooths out tick jitter. Tempo changes are followed within about 1 beat, and the result shows no audible wobble.
  - Start, stop, and song position pointer are followed.
  - Sync status is shown in the control bar and on controller displays (M5).

### Tuning Verifier — `src/tools/TuningVerifier`
- A tool for checking that a piece of hardware (or a plugin) **really plays Colundi in tune**:
  1. Send a MIDI note (using the device's retuning profile) and record its audio through the External Instrument's input.
  2. Detect the pitch with the YIN algorithm, with parabolic refinement.
  3. Repeat for every note in the range you choose.
  4. Report the error in cents for each note, with a chart, and flag notes off by more than a threshold (default 3 cents).
- **Latency check:** the same tool sends a click and measures how long it takes to come back. That sets the External Instrument's latency setting.
- **Bend-range check:** the tool sends a known bend and measures the result. A mismatch with the profile's range is flagged.
- It works on plugins too, internally with no audio interface. This gives a way to check whether an MTS-ESP-aware plugin follows the tuning.

## Tests
1. **Scanner**, using test plugins built in the repo with JUCE (in `tests/fixtures/plugins/`):
   - **Test plugins:** normal, crash-on-scan, hang-on-scan, latency-reporting, latency-changing, multi-out, sidechain, MIDI-out arpeggiator, MPE-capable, and one that allocates on the audio thread.
   - **Checks:**
     - crashing and hanging plugins end up on the blocklist with the right reason
     - a second scan with nothing changed takes almost no time
     - library records are correct
2. **Hosting:**
   - Plugin state survives save and reload.
   - Parameter groups become pages for N = 8, 16, and 32.
   - Configure mode saves and loads its choices.
   - A missing-plugin placeholder keeps the state exactly, and the plugin comes back intact after a reinstall.
   - A latency change is compensated: an impulse test shows the tracks lined up again, without a dropout.
   - Sidechain and multi-out routing render correctly.
   - The allocating test plugin shows up in the diagnostics.
3. **Isolation:**
   - The crash test plugin in each isolation mode: the app keeps running, the other tracks keep playing, and the device can be reloaded.
   - Same-block mode renders sample-for-sample identical to in-process.
   - Pipelined mode adds exactly one block of latency, compensated.
   - The handoff cost is benchmarked.
4. **MTS-ESP:**
   - A test MTS-ESP client in the test program reads back frequencies equal to the table (exact to `double` precision) for 12-TET, Colundi, and a user tuning.
   - Multi-channel mode gives each channel its own tuning.
   - Notes above Nyquist are filtered.
   - These tests are **skipped, with a clear notice, if the MTS-ESP library isn't installed**. The CI image installs it where the license allows.
5. **Exports:**
   - `.tun` frequencies are exact.
   - Colundi exported to `.scl`+`.kbm` and read back with M1's parser matches all 128 frequencies within 1e-6 cents.
   - MTS sysex bytes match example values computed from the MIDI Tuning Standard spec, including edge notes and the 14-bit fraction rounding.
6. **Hardware retuning (pure logic):**
   - The bend for every Colundi note matches the formula at ±2, ±12, and ±48.
   - The bend message is always sent before its note-on.
   - Channel choice gives no two sounding notes the same channel, and prefers the channel released longest ago.
   - Stealing sends the note-off first.
   - A full Colundi clip played through the MPE retuner and decoded back gives each note's intended frequency within 0.1 cent at ±2.
7. **MIDI out timing** (tagged `[timing]`, so it isn't run in Docker): using a loopback port (a Windows MIDI Services loopback, an ALSA virtual port on a real Linux machine, or a virtual port), measured jitter is ≤ 1 ms p99 over 10 minutes. The per-port latency offset is applied exactly.
8. **Clock:**
   - Clock out: the spacing between ticks matches the tempo within 0.1 ms on average, and song position pointer is correct after stop/continue.
   - Clock in: a simulated clock with jitter and tempo steps is followed within 1 beat with no overshoot past a set limit, and start, stop, and song position pointer are followed.
9. **Tuning Verifier:**
   - Run offline through a built-in Synth: it reports under 0.1 cent error for Colundi.
   - With a deliberately wrong bend range, the mismatch is flagged.
   - Latency measured through a simulated delay line is exact to within 1 sample.
10. **Performance:**
    - The overhead of hosting a do-nothing plugin is under 1 µs per block.
    - The sandbox handoff overhead is measured for each mode.
    - Both are added to the `lattice_bench` baselines (enforced as in M3).

## Definition of done
- [ ] Your installed VST3 (and CLAP) plugins scan safely in the background, and a broken plugin can't crash or hang the app.
- [ ] A plugin synth on a Colundi track plays the table frequencies through MTS-ESP. Its parameters are paged on Virtual Push, and on Push if you have one.
- [ ] Two tracks with different tunings drive two MTS-ESP plugins correctly at the same time (multi-channel mode).
- [ ] A crashing plugin in a sandbox mode leaves the rest of the set playing.
- [ ] A hardware synth (or, before one is available, a plugin standing in for one) is retuned to Colundi with MPE bend, and the Tuning Verifier reports every note within 3 cents (within 0.1 cent at ±2 bend on a synth that responds exactly).
- [ ] An External Instrument's latency is measured automatically and lines up with internal tracks.
- [ ] Clock out drives an external sequencer steadily, and clock in follows an external tempo.
- [ ] All tests pass in CI, and benchmarks are within budget on the reference machine.

## Risks
- **Plugins behave very differently from one another.** Mitigated by safe scanning, the isolation modes, diagnostics for each plugin, and a list of tested plugins kept in `docs/`.
- **CLAP hosting** may need our own host implementation. It's about the size of a medium feature, behind the existing format interface.
- **MTS-ESP:** license terms, the required system library, and the single-master rule. File and sysex exports and the CLAP tuning extension are the fallbacks.
- **Windows MIDI timing** without Windows MIDI Services depends on how precise the system's timers are. Measure on the reference machine before promising the 1 ms target there.
- **Isolation** is the most complex part of M6. It can come after the rest of M6 without blocking it.

## Interfaces kept for later milestones
- **M7 Maschine MK3:** its built-in audio interface simply appears as an audio device, and its MIDI mode works through M5's generic definition.
- **Post-MVP:**
  - The audio-input path from the External Instrument is the base for audio tracks and recording.
  - Link sync uses the same transport-following logic as clock in.
  - The CLAP tuning extension and the expression routing carry over to MPE recording.
