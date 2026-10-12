# Sub-plan M2: Engine + Session

Parent: `00-preliminary-plan.md`, milestone M2. Covers component #1 (project model, core part), #2 (audio engine), and #3 (clip and session engine).
Depends on: M0 (engine skeleton, RealtimeCheck, test runner) and M1 (`TuningTable`, `TrackTuning`, `TuningTestSynth`).

## Goal
A working Session view engine:
- MIDI tracks, each with an instrument (the M1 test synth for now)
- a grid of clip slots and scenes
- sample-accurate clip launching with launch quantization
- looping MIDI clip playback
- MIDI clip recording
- transport, tempo, and metronome
- a basic mixer (volume, pan, mute, solo, arm, meters)
- saving and loading the project to a file

A bare-bones debug UI drives it. The real Live-style UI is M4.

**Out of scope here:**
- audio tracks and audio clips
- arrangement view
- automation
- groove/swing
- return tracks and sends (they arrive with effects in M3)
- Repeat launch mode
- count-in
- multi-threaded track processing (moved to M3 phase 3e)
- Ableton Link

## Key design decision: build our own engine
Tracktion Engine (GPLv3 or commercial license, built on JUCE) already provides a DAW engine with a clip launcher. We **build our own** for three reasons:
- Session view behavior has to match Live closely.
- Colundi tuning has to reach deep into note handling.
- Push integration needs full control over engine state.

This is the most expensive choice in M2. If the schedule matters more than control, Tracktion Engine is the fallback.

## Threading model
| Thread | Owns | Talks to the audio thread through |
|--------|------|-----------------------------------|
| Message (UI) thread | `juce::ValueTree` document, `UndoManager`, the builder that turns the document into a snapshot | publishes snapshots with an atomic pointer swap; sends commands through an SPSC FIFO |
| Audio thread | the current `EngineSnapshot`, playback state (playheads, queued launches, active notes) | sends state and meters back through an SPSC FIFO plus atomics |
| MIDI input thread | — | `juce::MidiMessageCollector` (from M1), drained at the start of each audio block |

**Rules (enforced by M0's RealtimeCheck):**
- No allocation, locks, or frees on the audio thread.
- The audio thread only reads immutable snapshots.
- Playback state lives in arrays that are allocated in advance.

### `RtShared<T>`: generalizing the M1 tuning swap — `src/core/RtShared.h`
- M1's tuning handoff (atomic pointer swap plus a retire list) moves into a reusable template:
  - `publish(std::shared_ptr<const T>)` runs on the message thread.
  - `const T* acquire()` runs on the audio thread and doesn't block.
  - A timer clears the retire list once the audio thread has moved past the old pointer. The audio thread bumps an "epoch" counter at the end of each block, and the retire list compares against it, so we never rely on a guess about timing.
- `TrackTuning` is refactored to use it. Snapshots and clip data use it too.

## Part 2a: Project model — `src/core/model/`
The project model is a `juce::ValueTree` with typed wrapper classes, edited only on the message thread and always through the `UndoManager`.

```
LiveSet        tempo, timeSigNum/Den, globalLaunchQuantization, metronomeOn, selectNextSceneOnLaunch
 ├─ Tracks[]   MidiTrack: id, name, color, volumeDb, pan, mute, solo, arm, tuningId, noteOffset, highFreqPolicy
 │   └─ Slots[]  ClipSlot (one per scene): hasStopButton, optional MidiClip
 │        └─ MidiClip: name, color, startMarker, loopStart, loopEnd, looping, launchMode,
 │                     launchQuantization ("Global" | value), legato, followAction{A,B,chanceA,timeBars},
 │                     Notes[]: pitch, startBeat, lengthBeats, velocity, muted
 ├─ Scenes[]   name, color, optional tempo, optional timeSig
 └─ Master     volumeDb, pan
```
- Times are in beats as doubles. Note pitch is a MIDI note index from 0 to 127. Its frequency comes from the track's tuning, so in Colundi, note 47 means 440 Hz.
- Each track has an ID that stays the same when tracks are reordered. Tracks and scenes keep the same number of slots.
- Operations, each one undoable:
  - add, delete, duplicate, or move a track or scene
  - create, delete, duplicate, or move a clip
  - edit clip properties
  - add, remove, or edit notes
  - change mixer values
- **Serialization:** ValueTree to XML in a `.lset` file, with a `formatVersion` attribute and a migration hook. M4 adds the File menu and unsaved-changes tracking on top of this.
- **MIDI import:** `importMidiFile(path, slot)` reads a `juce::MidiFile` and creates one clip per track. This is how clips get content until M4 adds the piano roll.

## Part 2b: Engine graph, transport, mixer — `src/engine/`

### `EngineSnapshot` (immutable, built by `SnapshotBuilder` on the message thread)
- Tempo, time signature, quantization, and an array of `TrackSnapshot`.
- Each `TrackSnapshot` holds:
  - a pointer to its instrument processor
  - its tuning pointer
  - mixer values
  - an array of `shared_ptr<const ClipData>` for its slots
- `ClipData` holds the clip's notes, sorted by start time, already flattened into note-on/note-off event pairs for the loop region.
- Editing one clip rebuilds only that `ClipData` and makes a new snapshot that reuses everything else. This keeps rebuilds cheap while you edit a playing clip.
- Instruments are kept by track ID across snapshots, so a rebuild doesn't recreate synths or cut off sound.

### Device interface
- Instruments and (in M3) effects are `juce::AudioProcessor` subclasses. Hosted VST3/CLAP plugins in M6 are AudioProcessors as well, so built-in and third-party devices share one interface.
- In M2 every track's instrument is a `TuningTestSynth` wrapped as an `AudioProcessor`.

### Signal flow for each block
1. Drain the commands (launch, stop, transport, and so on) from the FIFO.
2. Drain live MIDI input and send it to armed tracks.
3. Split the block wherever something must happen exactly on a sample (a quantized launch, a stop, a loop wrap, a tempo change).
4. For each track: the clip player writes MIDI events at exact sample offsets into a `MidiBuffer` allocated in advance. The live input is merged in. The instrument processes it. Then come the track gain and pan (smoothed), followed by the peak meter.
5. Add the tracks into the master bus, apply master gain and pan, measure peaks, and add the metronome.
6. Advance the transport and bump the epoch counter.

Processing is single-threaded in M2. The snapshot layout keeps tracks independent, so a later milestone can add a thread pool.

### `Transport`
- The playing state, the beat position (`double`), tempo (20–999 BPM), and time signature.
- Beats per sample are `tempo / 60 / sampleRate`. The position is computed from an integer sample counter within each constant-tempo segment, not added up block by block, so it doesn't drift.
- Play / Stop / Continue.
- If a clip is launched while the transport is stopped, the transport starts and the clip starts at once (the transport begins at bar 1).
- Scene tempo: launching a scene that has a tempo sets the tempo at the launch point.
- Tap tempo is computed on the message thread (average of the last 4 taps, reset after 2 s).

### `Metronome`
- A short synthesized click: a higher-pitched accent on the downbeat, a normal click on other beats.
- Volume and on/off settings. It plays only while the transport runs.

### Mixer
- **Volume:** from −∞ to +6 dB, smoothed.
- **Pan:** constant-power pan law (−3 dB at center).
- **Mute:** silences the track; its clips keep playing in the background, as in Live.
- **Solo:** one track at a time by default. Ctrl+click adds to the current solo set (the debug UI uses a modifier key for this).
- **Arm:** sends live MIDI input to the track and allows recording.
- **Meters:** per-channel peak values in atomics. The UI reads them about 30 times a second and handles the falloff.

## Part 2c: Clip playback and launching — `src/engine/session/`

### Clip states (reported to the UI and, later, to Push LEDs)
`Empty`, `Stopped`, `QueuedLaunch`, `Playing`, `QueuedStop`, `QueuedRecord`, `Recording`

### `ClipPlayer` (one per track, state allocated in advance)
- Plays the current `ClipData` starting at the start marker:
  - It then loops from `loopStart` to `loopEnd`.
  - If looping is off, it stops at `loopEnd`.
  - It handles a loop wrap that falls inside one block, including several wraps when a loop is shorter than a block.
- Notes that run past `loopEnd` are cut at the wrap.
- **No stuck notes:** a fixed 128 × 16 table records which notes are sounding. Note-offs are sent at the exact sample whenever:
  - a clip stops
  - a clip is replaced
  - the loop wraps over a held note
  - the snapshot changes the clip under the playhead
  - the user presses stop all
  - the transport stops

### `LaunchScheduler`
- Launch quantization values:
  - None
  - 8, 4, 2, or 1 bar
  - 1/2, 1/4, 1/8, or 1/16 note
- A clip uses the global value unless it has its own override.
- The launch time is the next grid line at or after the moment the command arrives. With None, it is the next sample.
- One pending launch per track at most. A newer launch replaces the pending one.
- **Launch modes:**
  - **Trigger:** starts on press.
  - **Gate:** plays while the pad is held. The release is quantized like the launch.
  - **Toggle:** a second press stops the clip.
  - Repeat mode is deferred.
- **Legato:** the new clip starts at the old clip's position (taken modulo the new clip's loop length) instead of at its start marker.
- **Slot stop buttons:** launching an empty slot that has a stop button queues a stop for that track, quantized.
- **Scene launch:**
  - Launches every slot in the scene's row.
  - Empty slots with stop buttons stop their tracks.
  - Applies the scene's tempo and time signature if set.
  - Moves the selection to the next scene if "select next scene on launch" is on.
- **Stop all clips:** quantized. Pressing it a second time stops at once.

### Follow actions (simplified Live 11 style)
- Each clip has two actions, A and B. Choices:
  - None
  - Stop
  - Play Again
  - Previous
  - Next
  - First
  - Last
  - Any
  - Other
- Settings: `chanceA` (0–100%) and a follow time in bars and beats.
- Evaluated on the audio thread when the follow time is reached. The resulting launch goes through the same scheduler.
- Uses a seeded random generator inside the engine, so tests can check the results.

## Part 2d: MIDI clip recording — `src/engine/session/Recorder`
- **Starting a recording:** press Record while an empty slot on an armed track is selected, or launch an empty slot on an armed track (as in Live). The slot shows `QueuedRecord` and starts at the launch-quantization point.
- **Capturing notes:** input events are stored with their beat times in a buffer allocated in advance (4096 events per track). If the buffer fills, extra events are dropped and counted; nothing is reallocated. The message thread drains the buffer and turns the events into undoable note edits in the model.
- **Fixed length:** an optional preset clip length (1, 2, 4, 8, or 16 bars). When it is set, recording turns into playback automatically at the end.
- **Open length:** without a fixed length, stopping the recording rounds the clip length up to the next whole bar.
- **Overdub:** when the session record button is on and a clip is playing on an armed track, new notes are added to that clip.
- Notes still held when recording stops get a note-off at the stop point.

## Part 2e: Debug session UI — `src/app/debug/` (throwaway; replaced in M4)
- A transport bar with:
  - play and stop
  - tempo, with tap tempo
  - time signature
  - a global quantization drop-down
  - metronome and session-record toggles
  - the bar.beat.sixteenth position
- A grid of tracks × scenes made of buttons:
  - Each button's color shows the clip state, with blinking for queued states.
  - Clicking launches the clip.
  - Right-clicking offers: import MIDI file, delete clip, and clip properties (loop, launch mode, quantization, legato, follow actions).
- Scene launch buttons and a Stop All Clips button.
- One mixer strip per track:
  - a volume slider, a pan knob
  - mute, solo, and arm buttons
  - a meter
  - a tuning drop-down that reuses M1's component
- Buttons to add a track or a scene, and Save/Open (`.lset`).
- **No-hardware rule:** every engine command (launch, stop, scene, stop all, arm, record, overdub, transport) can be reached from this UI. The computer keyboard from M1 sends to armed tracks, so a whole session can be recorded and played with only a computer.
- **Live-style shortcuts:**
  - Space: play/stop
  - Shift+Space: continue
  - F9: record
  - Enter: launch the selected clip or scene
  - Arrow keys: move the selection
  - Ctrl+Z / Ctrl+Y: undo / redo
  - Ctrl+S / Ctrl+O: save / open
  - `M`: computer keyboard on/off
- The computer keyboard's note keys take priority. Shortcuts use keys and modifier combinations that don't clash with them.

## Tests (Catch2) — `tests/engine/`, `tests/core/`
All engine tests use an **`OfflineRenderer`**:
- It drives `AudioEngine`'s callback directly with no audio device.
- It records every MIDI event each instrument receives, together with its absolute sample position.
- It runs every scenario at **several block sizes (64, 37, 512, and a random size for each block)**, and the results must match exactly.
- It runs inside `ScopedAudioThread`, so any allocation on the audio thread fails the test.

1. **Transport:**
   - At 120 BPM and 48 kHz, 4 beats come out to exactly 96,000 samples.
   - A tempo change in the middle of a render puts the position exactly where hand calculation says it should be.
   - There is no drift after 1 hour of simulated time.
2. **Launch quantization:**
   - A launch at beat 1.3 with 1-bar quantization starts at sample 4 × 24,000 exactly.
   - All quantization values are covered.
   - A clip's own quantization overrides the global setting.
3. **Note timing:**
   - A note at clip beat 0.5 arrives at the exact sample.
   - Loop wraps work when they fall inside a block and when a loop is shorter than a block.
4. **No stuck notes:** in each case below, every note-on gets a matching note-off. The cases are:
   - stopping a clip in the middle of a note
   - switching clips
   - stop all
   - stopping the transport
   - a snapshot swap that removes the clip while it plays
5. **Launch modes:**
   - Trigger, Gate (release is quantized), Toggle.
   - Legato keeps the position.
   - A slot stop button and a scene launch with empty slots both behave correctly.
6. **Follow actions:** with a fixed random seed, a sequence of clips (Next, Other, Any, and so on) gives the expected launch order.
7. **Mixer:** solo works one track at a time and with Ctrl+click additions. A muted track is silent while its clip keeps playing. The pan law gains match the formula. Volume ramps without clicks.
8. **Recording:**
   - Fake MIDI input lands at the right beat times in a new clip.
   - Fixed-length recording turns into playback at the right time.
   - Open-ended recording rounds up to the next bar.
   - Overdub adds to the existing notes.
9. **Colundi through the engine:** a clip on a Colundi track with notes 12, 47, and 100 renders those fundamentals (59.9, 440, and 9480 Hz) within 0.5 Hz. This extends M1's render test so the pitch stays right on the way from clip to instrument.
10. **Model:**
    - Saving and reloading a `.lset` file gives a matching ValueTree.
    - Every operation can be undone and redone.
    - Importing a known `.mid` file produces the expected notes.
11. **Snapshots:** a stress test edits a playing clip 1,000 times a second for 10 s. It runs with no crash, no data race (TSan on macOS CI), and no free on the audio thread.

## Definition of done
- [ ] Create 4 tracks × 8 scenes and import MIDI files into slots.
- [ ] Launch clips and scenes from the debug UI with quantization, and hear them loop in time with the metronome.
- [ ] A track set to Colundi plays its clip at Colundi pitches.
- [ ] Record a MIDI clip from a keyboard on an armed track, then overdub it.
- [ ] With no MIDI or controller hardware connected, use only the mouse, keyboard shortcuts, and computer keyboard to build, record, launch, and save a 4-track session.
- [ ] Volume, pan, mute, solo, and meters behave as described.
- [ ] Save, quit, reopen: the set comes back exactly as it was.
- [ ] All tests pass at every block size. The RealtimeCheck stays silent during long playback sessions.

## Interfaces kept for later milestones
- **M3 devices:** each track's device chain fills the "instrument processor" spot in `TrackSnapshot`. Return tracks and sends are added to the snapshot and the mixer.
- **M4 UI:** it reads the same model and the same clip-state feed, and adds the piano roll and the File menu on top of the existing serialization.
- **M5 Virtual Push and Push hardware:** both send the same commands as the UI and each other (launch, stop, scene, arm, record). Clip states drive the pad LEDs one-to-one. Sample-accurate launching means pressing a pad behaves exactly like clicking a clip.
- **M6:** hosted plugins drop into the AudioProcessor spot. MIDI clock output reads the `Transport`.
