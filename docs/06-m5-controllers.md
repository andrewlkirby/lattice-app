# Sub-plan M5: Controllers (Virtual Push, Push 2/3, Generic MIDI Controllers)

Parent: `00-preliminary-plan.md`, milestone M5 (component #8).
Depends on:
- M2: engine commands, clip states, recording
- M3: `ParameterPager`, Drum Rack, voice pitch design
- M4: `CommandRegistry`, `EngineStateFeed`, highlight sets, the Preferences → Controllers tab, MIDI Map

## Goal
A controller layer that works with:
1. **Virtual Push:** on screen, with an adjustable size.
2. **Ableton Push 2 and Push 3** (Push 3 in controller mode): full native drivers, including the color display.
3. **Generic MIDI controllers:** grid controllers (Launchpad, APC, and similar), pad controllers, and keyboards with knobs. They are described by **surface definition files**, which can also be created with a built-in "learn" wizard.

All three run the **same device-independent mode logic**: Session, Note (including the Colundi layout), Drum, step sequencer, Device, Mix, Browse, and Clip. That logic adapts to each device's grid size, encoder count, and display.

Playing pads must feel as tight as a dedicated instrument. **Pad-to-sound latency is a quality target.**

**Out of scope here:**
- native Maschine MK3 support (M7; its MIDI mode can already use a generic definition)
- recording and editing MPE in clips: live MPE playing works in M5, recording it waits until after the MVP
- automation and arrangement modes
- Push 1 (possible later as a generic definition with a text display)
- scripting inside definition files (they are JSON only for now; see the Risks section)

## Starting point (updated 2026-11-02, after M4)

### What M2–M4 leave to build on
- **Used as they are:**
  - `CommandRegistry`: every action has a stable ID, so the modes perform commands rather than calling into views;
  - `SessionController` for launches, recording, tap tempo and note previews, and `LiveSet` with its `UndoManager` for edits;
  - `SessionSelectionModel`, the shared selection (track, scene, and whose devices are shown);
  - `EngineStateFeed`: slot states, track play states, the transport and meters, once per display refresh, calm at 4 Hz;
  - `ParameterPager::pagesFor(device, encoders)`, for any encoder count;
  - per-track and per-clip highlight sets (`LiveSet::effectiveHighlight`), the tuning registry and `TrackTuning`;
  - MIDI Map's gestures (one undo step per turn, ended at rest), which encoder turns follow too;
  - the Preferences → Controllers tab (empty so far).
- **What has to change:**
  - **Live MIDI isn't timestamped yet.** `MidiInputQueue` plays everything at sample 0 of the next block, and its producers share a mutex. The fast path needs lock-free queues, one per producer thread, and each event placed at the sample its timestamp gives (5b). The command queue (`MpscQueue`) is the same, for launches from a MIDI thread.
  - **MIDI Map reads remote input on the message thread** (`takeRemoteMidi`). Surface ports bypass it: their driver reads its input directly, and only controls a surface leaves unassigned go on to MIDI Map (5e).
  - `ComputerKeyboardInput` stays as it is. It keeps working alongside every controller.

### Order of work
- **Hardware comes last.** The modes are built and tested on `FakeSurface` and Virtual Push first, because no controller has been bought yet. Each driver is then a thin layer under modes that already work.
- **Phases:** 5a → 5b → 5c → 5d → 5e → 5f → 5g.
  - 5c and 5d both depend only on 5a and 5b, so they can swap places. 5e and 5f only depend on 5c.
  - **Bringing up the purchased device** ("Hardware choice", below) goes in whenever it arrives, after its driver's phase.
- **Each phase** ends the same way as in M4:
  - its tests pass on Windows and Linux;
  - the performance targets it touches are checked (the idle budget with Virtual Push open, LED response, pad latency);
  - the user does a short hands-on check.

#### 5a: Foundation, Virtual Push, Session mode
- **`src/control/surface/` holds the device-independent types:**
  - `SurfaceCapabilities` and the `Role` names;
  - `SurfaceInput` events: pad down, pressure and up (row, column, velocity); button down and up (role); encoder turn (index, delta in steps) and touch; strip;
  - `LedFrame`: each pad's and button's color and animation;
  - `DisplayFrame`: what each display column shows (structured data; it's drawn in 5c).
- **`SurfaceDriver`** is the interface every driver implements: its capabilities, an input callback, and LED and display frames out. `FakeSurface` implements it for the tests. **Capability presets** for every candidate device and the Virtual Push sizes are written now (see "Hardware choice").
- **`SurfaceController` and `ModeStack`** run on the message thread. They read a `SurfaceContext`: the commands, the session, the selection, and the feed.
  - Modes are plain classes, so tests drive them headlessly.
  - LEDs are only re-computed when the feed or the selection reports a change. Drivers get the differences only.
- **Session mode** and the **global controls**: play, record, session record, metronome, tap tempo, undo/redo (with Shift), new, stop all, and the Shift overlay.
- **Virtual Push** (`src/control/virtual/`):
  - a panel in the shell (View menu, or a shortcut) that can also open as its own window;
  - grid size, encoder count and the button groups shown come from Preferences → Controllers;
  - mouse input: velocity from the click position, right-click latches a pad;
  - it repaints only the pads whose LEDs changed. Blinking and pulsing follow the transport's beat from the feed, so a still set costs nothing.
- **Tests:**
  - Session mode on every candidate size: the pad→slot mapping, navigation, page jumps, LED colors and animations for each clip state, and the overview;
  - graceful degradation, with only a grid and no buttons;
  - the LED diff;
  - the idle budget with Virtual Push shown.
- **Hands-on check:** launch clips and scenes and use the transport from Virtual Push, at 8×8 and at 8×12.

##### As built (5a, 2026-11-02)
- **Where things are:**
  - `src/control/surface/`: `Surface.h` (roles, capabilities, input, LED frames, `SurfaceDriver`), `Presets` (every candidate device, Virtual Push, grid only), `Mode.h` (`SurfaceContext`, `ModeHost`, `Mode`), `SurfaceController`;
  - `src/control/modes/SessionMode`;
  - `src/control/virtual/VirtualPush`.
  
  They're built into `lattice_ui`, since the modes use its commands, feed and selection.
- **LEDs:**
  - A light is a colour (`0xRRGGBB`) plus an animation. A pulse takes one beat; a blink is on for the first half of each half-beat.
  - Colours for states come from the theme (play, record, arm, mute, solo). White stays white, since it's the light's colour.
  - The controller works the frame out again only when something changes: a feed tick it listens to, the set, the selection, or a held button. It then sends only the differences.
- **Global buttons (the controller's):**
  - Play: play/stop. Shift+Play continues.
  - Record: session record.
  - Undo; Shift+Undo redoes.
  - New: a clip in the selected slot. Shift+New: a scene.
  - Stop All, or Shift+Stop Clip (as on Push 2).
  - Metronome, Tap Tempo, Shift, and the mode buttons. A mode that isn't built yet is unlit.
- **Session mode as built:**
  - **Pads and the selection:**
    - Pressing a pad selects its slot. With Shift it only selects.
    - The first press on an empty slot of a track that isn't armed only selects it, which auto-arms it in the app. It stops the track if the slot has a stop button. The next press records.
    - Launching a scene doesn't select it, since that would take the selection, and auto-arm, off the track being played. Shift+scene selects it.
  - **Delete and Duplicate held:** they act on the clip, or on the scene from a scene button. Each is one undo step.
  - **Track buttons** (trackSelect, or the buttons under a display on Push): they select the track. With Stop Clip, Mute or Solo held they stop, mute or solo it instead; Shift+Solo adds to the soloed tracks. A device without track buttons gets a row of track pads at the bottom while one of those three is held.
  - **A grid with no buttons:**
    - its last column launches scenes;
    - the box follows the selection made on screen;
    - everything else stays on screen.
  - **Columns** are the tracks the Session view shows, so a folded group hides its tracks. A group's pad launches its row, as in the Session view.
  - **The overview** (Session held) shows the current page white, a page with a playing clip in the play colour, and dim pages with or without clips. A pad moves the box there, and the selection to the page's first slot.
- **Virtual Push:**
  - **Where it shows:**
    - By default it's docked at the window's right, full height, with its width dragged at its left edge; the default is 35% of the window.
    - "Virtual Push in Its Own Window" moves it to a window of its own, which remembers its place.
    - It and its controller exist only while shown.
    - Toggle it with View → Show Virtual Push (Ctrl+Alt+P).
  - **Layout:** the grid, a scene button per row and a track button per column, with the other buttons in two columns on the left (all 30 roles). Everything is sized from one unit, the largest that fits, at the top of the panel.
  - **Drawing:** unlit pads and buttons are dark in every theme, as on a device, so a light's brightness reads the same on the light theme.
  - **Input:**
    - Velocity comes from where a pad is clicked (top: loudest), or is fixed in Preferences → Controllers, which also sets the rows and columns (4–16).
    - A right-click latches a pad or a button, so Shift, Delete or Mute can be held with one mouse.
    - Each touch is its own press.
  - **Animation:** a display-refresh repaint runs only while a blinking or pulsing light is shown. A still set adds no timers.
- **Tests:**
  - `tests/control/SessionModeTests.cpp`: Session mode on every preset (Push 2/3, Launchpad Pro MK3, APC64, APC40 MKII, Maschine MK3, Virtual Push at 8×8, 8×12, 16×16 and 4×4, and grids only), and the global buttons.
  - `tests/control/VirtualPushTests.cpp`: the layout at six sizes in three shapes, clicks, latches and velocity, and the shell docking it, floating it and rebuilding it.
  - A `virtual-push` render snapshot in each theme.
  - `lattice_ui_bench --idle N --virtual-push` measures idle CPU with it shown.
- **Checks (2026-11-02):**
  - The user's hands-on check passed.
  - Quiet-machine idle with Virtual Push shown: 0.25-0.27% above a bare JUCE window (budget 0.3%). Without it, in the same session, 0.25-0.29%, so it adds nothing measurable (`perf/reference-machine.md`).

#### 5b: Playing: the fast path and Note mode
- **Engine:**
  - live MIDI and commands from lock-free queues, one per producer, with no mutex;
  - events placed at the sample offset their timestamp gives, one block after they arrive, so the delay is constant and there's no jitter;
  - the input latency probe covers it.
- **`PadNoteMap`**, through `RtShared`: a driver's input thread plays pads without the message thread.
- **Note mode layouts:** Colundi/table (with its presets), In Key, and Chromatic. Also the Scale/Tuning menu overlay, octave buttons, the highlight and anchor colors, and duplicate pads lighting together.
- **Expression:** a velocity curve, polyphonic and channel pressure, and the touch strip modes.
- **Tests:** the layout formulas and colors; timing through `OfflineRenderer`; a stalled UI (500 ms) not delaying notes or launches; and RealtimeCheck on the input threads during a pad fuzz.
- **Hands-on check:** play the Colundi table from Virtual Push.

##### As built (5b, 2026-11-03)
- **Engine:**
  - **Queues:** `MpscQueue` (commands) and `MidiInputQueue` (live MIDI) are lock-free for any number of producers: a bounded queue whose cells carry sequence numbers (Vyukov's). A producer claims a cell with one compare-exchange and never waits for another producer or for the audio thread.
    - **Change from the plan:** one queue shared by every producer, not one per producer. The OS doesn't promise one thread per MIDI input, so a per-producer queue couldn't be guaranteed a single producer. The effect is the same: no mutex anywhere.
  - **Timing:** each live message keeps its arrival time.
    - Each callback places it at `block length − its age in samples`, so it plays exactly one block after it arrived: a constant delay, whatever the callback's own jitter.
    - Older ones (a late callback) play at the block's start, and so does everything offline.
    - A track's events are merged into time order in place, without allocating, only when out of order.
    - `AudioEngine::process` takes the callback's start time; `OfflineRenderer::useClock` gives tests a device clock.
  - **Latency probe:** it now measures from arrival to the moment the message's sample plays, counted from the callback's start. That's one block for every event, with the driver's output latency on top.
  - **`AudioEngine::queueCommand`:** real-time safe. `sendCommand` also wakes what polls the engine, which can lock, so the fast path calls that afterwards.
  - **Still to do (5e):** the JUCE MIDI-input callback still takes two short locks: the input's source number and MIDI Map's inbox. Its notes go into the lock-free queue. Lock-free tables for both come with 5e's generic MIDI drivers, which replace that path.
- **The fast path (`PadPlayer`, `PadNoteMap`):**
  - The controller makes a `PadNoteMap` from the current mode on each refresh and publishes it through `RtShared` only when it changed. Each pad has one of:
    - a note;
    - a command, with an optional release command;
    - nothing: the message thread handles it.
  - The map also holds the surface's source and channel, the velocity and pressure tables, the strip mode, and the modifier buttons.
  - **How drivers use it:** a driver's input goes through `PadPlayer::handle` on its own thread, then to the controller marked `played`. Virtual Push and `FakeSurface` do both through `SurfaceDriver::deliver`.
  - **Modifiers:** while a modifier is held (tracked on the input thread), pads go to the message thread.
  - **Notes:**
    - A pad remembers the note it started, so its release ends that note even if the layout changed in between.
    - A second pad on a held note plays it again (off, then on), and the note ends with the last release.
    - `releaseAll` ends everything when the driver goes away.
  - **Pressure:** polyphonic sends each note its own. Channel sends the most any held pad is pressed, and goes back down as pads are let go. A device without aftertouch sends none.
  - **Strip:** pitch bend springs back to the middle on release, and the mod wheel stays put; both are on the fast path. Scroll goes to the mode.
  - **Session mode:** a slot pad gets `SessionController::directLaunch`'s launch (with a release for Gate clips) or its stop button. Scene buttons, and the scene column of a grid without them, launch their scenes (`PadNoteMap::buttons`).
    - It gives nothing for group slots, armed empty slots and recordings; those still go through the message thread.
    - The modifiers are Shift, Delete, Duplicate, Stop Clip, Mute, Solo and Session.
    - A played press only selects, so it doesn't launch twice. A stale clip ID is ignored by the engine.
  - **Routing:**
    - Each surface has its own source number (`lattice.surface.<name>`).
    - Tracks listening to all inputs (the default) hear it when armed or monitoring In, as in Live. There's no target track in the map: auto-arm puts the selected track there.
- **Note mode:**
  - **Layouts** (`NoteLayout`, plain functions):
    - **Table:** `origin + column×step + row×rowOffset`. Presets: Sequential, and Overlap 3/5/7 (`columns − n`). Fine −1/+1 row steps.
    - **In Key:** 14 scales, rows 4ths/3rds/Sequential, the root lit.
    - **Chromatic:** rows a 4th apart, notes outside the scale dimmed.
  - **Choosing a layout:** a table tuning offers only Table; 12-TET offers all three, In Key by default. The default Table origin is the computer keyboard's default page.
  - **Saving:** the layout is kept on the track's node (`noteLayout`, not undoable, no engine rebuild), tagged with its tuning. When the track's tuning changes, it starts from that tuning's default.
  - **Colours:**
    - **Table:** anchors in the track's colour, the track's highlight set white, the rest dim (`padDim` 0.22). Entries the track can't play are unlit, and red while pressed.
    - **12-TET:** the root in the track's colour, the scale white.
    - **A pressed pad:** green, together with every pad playing the same note.
  - **Buttons:**
    - Octave up/down, or the up/down arrows, move by a row or a page (Table) or an octave. With Shift they move by one entry, or by one semitone of the root in In Key.
    - Left/right and the track buttons choose the track.
    - Layout cycles the layouts.
  - **Scale/Tuning menu (Scale button):** shown on the pads until 5c's display renderer.
    - Each setting gets a row of choices: Tuning (an undoable change of the track's tuning), Layout, Root and Scale (12-TET), Rows, and Octave buttons (Table).
    - The pads are labelled on Virtual Push, and the status line shows the choice touched. The arrows scroll it on small grids.
  - **Status line:** each pressed note's entry or name and its Hz; otherwise the track, tuning, layout, origin and the number of highlighted entries.
- **Words on devices (`SurfaceText`):** pad labels and a status line, which Virtual Push draws and hardware ignores until 5c.
- **Virtual Push:**
  - A touch strip left of the grid (`stripAt`), and a status line above it.
  - Dragging up from a held pad is pressure: a pad's height is all of it, and a pen's own pressure is used if it has one.
  - Labels are shown when the pads are big enough.
  - With a fixed velocity, it has no velocity curve: it plays as set.
- **Preferences → Controllers:**
  - velocity curve (Softest…Hardest);
  - pad pressure (Off, Polyphonic, Channel) and its threshold;
  - touch strip (Pitch bend, Mod wheel, Scroll).
  - A change applies at once; only a new grid size or velocity rebuilds Virtual Push.
- **Tests:**
  - `tests/control/FastPathTests.cpp`:
    - the curve and threshold;
    - `PadPlayer`'s notes, duplicates, releases after a layout change, modifiers, pressure and strip;
    - placement one block after arrival at 64 and 512 samples (and the latency probe);
    - four producer threads with nothing lost;
    - a 500 ms stall of the message thread delaying neither a note nor a launch (under 100 ms with 10 ms blocks in real time);
    - RealtimeCheck on an input thread through a 40,000-event pad fuzz while maps are republished;
    - Session mode's direct launches.
  - `tests/control/NoteModeTests.cpp`:
    - the formulas and presets, octave limits, saving;
    - the Colundi and 12-TET colours, duplicates lighting together;
    - the menu, including scrolling on 4×4;
    - every preset device, the transport lights, and notes reaching the armed track.
  - Virtual Push: strip, pressure and text tests.
  - A `virtual-push-note` render snapshot in each theme.
- **Checks (2026-11-03):**
  - The user's hands-on check passed: the Colundi table played from Virtual Push.
  - From the check: docked Virtual Push was hard to read at half the window. It can now widen until the main view is 16 control heights wide (its pads size to the width); the window form (View menu) goes full screen.

#### 5c: Display and encoders: Device and Mix modes
- **The display renderer,** shared by Virtual Push and Push: it draws a `DisplayFrame` into an image at the device's size, in one column per encoder, with a detail view while an encoder is touched.
- **Encoders:**
  - Device mode (including rack navigation and macros first);
  - Mix mode;
  - an encoder turn is one gesture, so a turn is one undo step.
- **Tests:** the pages match `ParameterPager` for 8, 16 and 32 encoders, plus display snapshots.

##### As built (5c, 2026-11-03)
- **Where things are:**
  - `src/control/surface/Display.h` (`DisplayFrame`: cells, the labels along the display, the detail, a message; `displayText` for text displays);
  - `src/control/display/DisplayRenderer` (drawing; an image at a device's size);
  - `src/control/surface/EncoderBank` (what each encoder edits, and its gestures);
  - `src/control/modes/DeviceMode`, `MixMode`, `PadFaders.h`;
  - `src/ui/mapping/Targets` (MIDI Map's address-to-binding code, now shared with the encoders).
- **Two modes at once:**
  - On a device with encoders, a pad mode (Session or Note) runs on the grid and an encoder mode (Device or Mix) on the encoders, the display and the buttons along it, as on a Push. It starts in Device.
  - The buttons along the display go to the encoder mode first, everything else to the pad mode first. The Mode buttons light for both.
  - **Graceful degradation:** on a device without encoders (a Launchpad), Device and Mix take the grid. Its columns are faders (lit from the bottom, or from the middle for pan), the scene buttons choose pages or what to mix, and the arrows move along the chain or the tracks.
- **Encoders (`EncoderBank`):**
  - Each encoder is given a value's address (`device:<id>/<parameter>`, a track's volume or pan, a send) and edits it through the same bindings as MIDI Map and the window's knobs.
  - **One turn is one undo step:** from touch to release, or until the encoder rests for 400 ms on a device without touch. The step is named with the value, as a mouse drag's is.
  - **How far a turn moves:**
    - a whole range takes `encoderResolution` steps (a capability; 200 by default, unverified per device);
    - Shift makes it ten times finer;
    - a value with up to 24 settings (a switch, a choice, a small whole number) moves one setting every 10 steps, never sooner.
  - An absolute control (`encoderSet`: APC40, APC64) picks the value up before moving it.
- **Device mode:**
  - **Which device:** the device selected in the shared selection. `SessionSelectionModel::selectDevice` now holds it, and the Device view writes and follows it, opening the rack chains a device is in. With none selected, it uses the first device of the track (or return, or master).
  - **Pages:** `ParameterPager::pagesFor` for the encoder count. A rack's pages are its macros in use, read from the set (`RackDevice::macroPages`), with their names and colours.
  - **Upper buttons:**
    - they show the devices of the chain, in groups as wide as the buttons;
    - pressing the selected rack again shows its chains, and choosing a chain goes into it;
    - inside a rack, the first button ("‹ Rack") goes back out;
    - an empty chain says so.
  - **Lower buttons:** the device's On switch (undoable), then its pages. Each device remembers its page.
  - **Display:** each parameter's name, value and an arc (drawn from the middle when bipolar), with the devices above and the pages below. A touched encoder is shown three cells wide, with its range.
- **Mix mode:**
  - **Upper buttons:** Volume, Pan, Sends (press again for the next return's) and Track (the selected track's volume, pan and every send).
  - **Which tracks:** the tracks in view from the Session box. The box follows the selection under any pad mode, so Note mode moves it too.
  - **Lower buttons:** select tracks; with Stop Clip, Mute or Solo held they stop, mute or solo them, through Session mode's track buttons.
  - **Display:** each track's name in its colour, the value, an arc and its meter, drawn as the mixer draws its meters. Meters are read only while Mix is shown, in 64ths, so a still level doesn't redraw the display.
- **The renderer (`DisplayRenderer`):**
  - **Layout:** label strips above and below the cells, `across` cells to a row (Push: 8). Everything is sized from the bounds: a Push-shaped cell is 120 × 112, and each strip is 24.
  - **Theme:** dark in every theme, as a device's screen is. The theme gives the accent and meter colours, and the high-contrast theme gets brighter text.
  - `render()` gives an image at a device's size, for the Push driver (5f). Text displays get `displayText` (the touched encoder's name and value, otherwise the title and the cells).
- **Virtual Push:**
  - **Encoders setting** (Preferences → Controllers): none, 8, 16 or 32, default 8. With encoders it shows a display above the grid, as wide as the whole panel, with cells in rows of 8 (or the grid's width, if wider), and a button above and below each column.
  - **The cells are the encoders:**
    - pressing a cell touches its encoder;
    - dragging turns it: a whole range is three pads' height, and Shift (the key, or the button) makes it ten times finer;
    - the mouse wheel turns it by a fortieth of the range for each notch.
- **Scale/Tuning menu:** it stays on the pads, which work on every device. It can move to the display once a pixel display's driver exists to check it on.
- **Tests:**
  - **`tests/control/DeviceMixTests.cpp`:**
    - **Device:** pages equal to `ParameterPager`'s for 8, 16 and 32 encoders, and their addresses and cells; rack macros (12 in use: two pages on 8 encoders, named and coloured); choosing devices shared with the selection; the On switch and its undo; per-device pages; a track with no devices; going into a rack, choosing a chain, an empty chain, and back out.
    - **Turns:** a turn as one undo step; Shift; rests; detents; absolute pickup.
    - **Display frames:** a frame only sent when it changes; the detail on touch; text lines.
    - **Mix:** volumes, pans, sends cycling, the Track page following the selection, the lower buttons with modifiers, and the box following the selection.
    - **Without encoders:** faders on a Launchpad Pro, including their LEDs and undo.
    - **Running together:** both modes at once and their lights.
    - **Every candidate device:** Device and Mix usable and sized from what it has.
  - **Virtual Push:** the layout with 0, 8, 16 and 32 encoders at every size (cells inside the display, nothing overlapping), and drags, Shift and the wheel turning encoders.
  - **Snapshots:**
    - the renderer at 960 × 160 and 1920 × 320 in every theme: a device page, a touched encoder, Mix with meters, and no devices;
    - `virtual-push-device`: the 16-macro rack, with an encoder touched;
    - the earlier Virtual Push snapshots, redrawn with the display.
- **Checks:**
  - All 483 fast tests pass on Windows; all 472 on Linux (warnings as errors), with its snapshot references written and looked at. Callgrind: the engine benchmark -0.88%, the UI benchmark -0.02%.
  - The user's hands-on check passed (2026-11-04): Device mode, Mix mode, and pad faders with no encoders. From the check: Find now knows the Controllers tab's settings (it had missed them), with a test that every labelled Preferences row has a Find entry.
  - Idle with Virtual Push shown, measured on a busy machine: 0.42% above a bare window with it shown, 0.48% without it in the same session. The display adds nothing measurable, but the busy machine puts both over the 0.3% budget. A quiet-machine run waits for the user's go-ahead.

#### 5d: Drum, the step sequencers, and the Clip and Browse modes
- The Drum layout and its zones, the melodic step sequencer, and the Clip and Browse modes, with previews on the M4 preview bus.

##### As built (5d, 2026-11-04)
- **Where things are:**
  - `src/ui/clip/Steps` (a clip's notes as steps; a held step's bindings);
  - `src/ui/clip/ClipBindings` (the Clip view's values, shared with Clip mode);
  - `src/ui/clip/ClipActions` (Double Loop, Quantize and the set's Quantize and Fixed Length settings);
  - `src/control/modes/StepSequencer` (`SequencerZones`, the steps and the loop selector), `ClipMode`, `BrowseMode`, `SelectedClip.h`;
  - `src/control/surface/BrowseHost.h` (what Browse mode drives; the shell implements it).
- **Steps (`ui::steps`):**
  - A note shows on the step nearest its start, so a note nudged off the line, or drawn off it in the piano roll, still shows on its step. A step can be on, muted, tied (a longer note sounds through it) or off.
  - Pressing an empty step adds a note at once, a step long at velocity 100. A step with notes loses them when it's let go, unless it was held for half a second or edited meanwhile, so holding a step to edit it never deletes it. With no clip, the first step makes one in the selected slot, a page long. Each is one undo step.
  - **Held steps:** while steps are held, the encoders and the display are theirs: Velocity, Length (in steps) and Nudge (up to just under half a step either way, so a note stays on its step). The values go through `notes:<clip>/<step length>/<pitch>:<step>,…/<field>` addresses, so a turn is one undo step, as every encoder's is. Several held steps are edited together.
- **Which clip:** the selected slot's, or (when it's empty) the clip the track is playing. That's the Clip view's clip too, so the screen and the controller edit the same one. The step sequencers, Clip mode and the Double Loop and Quantize buttons all use it.
- **Zones (`SequencerZones`), from the grid's size:**
  - **Drum:** the steps along the top half (32 on 8 × 8, 48 on 8 × 12, 128 on 16 × 16), with the pads below on the left (4 × 4 on 8 × 8) and the loop selector on the right. Layout switches to the pads filling the grid (64 on 8 × 8). A grid smaller than 6 × 6 (4 × 4) shows the pads, or the steps over a row of pages, and Layout switches between them.
  - **Melodic:** a row for each note above, a column for each step, and the loop selector along the bottom (one row, or a row for every 8).
  - A test checks that every candidate device's grid is covered exactly once.
- **Drum layout:** used on any track whose chain has a Drum Rack.
  - **Pads:** they play on the fast path. The last one played is the one whose steps are shown, and it lights white. Pads with something on them are in their chain's colour (or the track's).
  - **Moving the pads:** the first pad on the grid is the rack's first pad on show, shared with the Device view. Octave up and down move the pads a page, and with Shift a row.
- **Melodic sequencer:** Layout cycles through the tuning's layouts and then the sequencer, so Colundi goes Table, Sequencer, and 12-TET goes In Key, Chromatic, Table, Sequencer. The Scale menu has it too.
  - **Rows:** each row is the layout's next note: a table entry (Colundi), a scale degree (In Key) or a semitone. Rows with anchors (or the root) are tinted, and unplayable rows are dark. Each row's first pad is labelled with its note.
  - **Moving the rows:** Octave up and down move the rows a page of rows (a table) or an octave; the arrows move them one row.
- **Step length:** 1/4, 1/8, 1/8T, 1/16 (the default), 1/16T or 1/32, chosen in the Scale menu and kept with the track's layout.
- **Loop selector:** a pad per page. Pressing a page loops it, and holding one while pressing another loops from the first to the second (one undo step). Shift+page only shows that page. While the clip plays, the page in view follows the playhead until a page is chosen with Shift.
- **Lights:** steps are lit in the track's colour, muted and tied steps dimmer. Empty steps inside the loop are dim, and steps past the loop end are dark. The playing step is green, and a held step is white.
- **Clip mode:** an encoder mode, like Device and Mix.
  - **Values:** the same bindings as the Clip view's panel (`clip:<id>/<value>` addresses), so a turn is the same undo step, with the same name, as a drag on the panel.
  - **Pages:** Loop (on, start, position, length, end), Launch (mode, quantization, legato, follow actions A and B, chance, time), Signature, and Quantize (the grid, the amount, and Fixed Length). They are packed into pages as wide as the encoders: three pages on 8 encoders, two on 16, one on 32.
  - **Upper buttons:** choose the page.
  - **Lower buttons:** Double Loop (Push's: the notes copied after the loop and the loop doubled), Loop × 2, Loop ÷ 2, Quantize, and Duplicate (into the next empty slot below, which then gets selected).
  - **Without encoders:** the columns are faders over a bottom row of the actions, and the scene buttons choose the page.
- **Global buttons, in any mode:**
  - **Double Loop and Quantize** act on the selected clip.
  - **Fixed Length** turns fixed-length recording on and off, back to the length it had. Shift steps through 1, 2, 4, 8 and 16 bars. Each change is undoable.
  - **Quantize settings:** the grid and amount Quantize uses are kept in the set (not undoable, as a view setting), and Clip mode's Quantize page edits them.
- **Browse mode:** drives the window's browser (`BrowseHost`). The screen's browser follows it, selecting a row plays its preview on the preview bus as a click does, and Load inserts where the Device view is looking, by its rules. A surface without a browser has no Browse mode, and its button stays dark.
  - **Display:** the sources (a quarter of the display) and the rows of the chosen source, as lists (`DisplayList`). Each list keeps its selection in the middle; about five rows fit on a Push.
  - **Encoders:** the encoders over the sources choose a source, and the rest move along the rows: a row every 10 steps, a page with Shift. An absolute control spans the whole list.
  - **Upper buttons:** the categories.
  - **Lower buttons:** Load, Open/Close (a folder, or a device's presets), Back, Preview (on or off, the browser's own switch), and Hot-Swap (the next Load replaces the selected device).
  - **Without encoders:** the rows go on the grid, a row of pads each. A press selects (and previews) a row, its last pad loads it, the arrows move and open, and the scene buttons choose categories. With encoders, the arrows stay the pad mode's, as in Device and Mix.
- **Display renderer:** draws lists in place of the cells. Text displays show each list's selection.
- **Tests:**
  - **`tests/control/StepSequencerTests.cpp`:**
    - steps' states, toggling, and the held-step bindings with their undo;
    - the zones on every grid;
    - the Drum layout: pads on the fast path, choosing the pad, steps making a clip, removing a step, holding a step, and the undo steps;
    - the loop selector and Shift, and moving the pads;
    - held steps taking the encoders and the display;
    - the melodic sequencer's rows, moves and step lengths;
    - the playhead while the clip plays.
  - **`tests/control/ClipBrowseTests.cpp`:**
    - Clip mode: its pages for 8, 16 and 32 encoders, a turn as one undo step named as the panel's, the actions, the global buttons, Fixed Length, and faders without encoders;
    - Browse mode, through a real window: categories, scrolling, Load, Open, Back, Preview, hot-swap, and the grid without encoders.
  - **Snapshots:** `virtual-push-drum`, `virtual-push-clip` and the renderer's `display-browse`. The earlier Virtual Push snapshots were redrawn for the buttons that now light: Layout (it reaches the sequencer), Clip, Fixed Length, Double Loop and Quantize.
- **Checks:**
  - All 496 fast tests pass on Windows; all 485 on Linux (warnings as errors), with its snapshot references written and looked at. Callgrind: the engine benchmark -0.88%, the UI benchmark -0.03%.
  - The user's hands-on check is next.
- **During the hands-on check:**
  - **Steps with only a track selected:** clicking a track's header selects no slot. The first step made a clip in the first slot, but the sequencer looked in the selected slot (none), so each step made the clip again and only the last note stayed. Steps now go into the track's first empty slot, which gets selected, and a clip already on the track is never replaced. A test clicks the way the user did.
  - **A walkthrough** (`tests/control/WalkthroughTests.cpp`, hidden: run `"[.walkthrough]"`): it clicks Virtual Push in a real window at 8 × 12, with and without encoders, as a person would. It loads the Synth Kit from Browse mode, then goes through the steps, the playhead, the loop selector, Octave, Layout, a held step, a Colundi preset's sequencer, the Scale menu and Clip mode. With `LATTICE_WALKTHROUGH_DIR` set, it saves an image at each step to look at.
  - **Labels without encoders,** found by looking at those images:
    - Clip mode's fader columns had no names, so each top pad now shows its value's name and value.
    - The scene buttons' jobs had no names, so a mode can now label a button (`SurfaceText::buttons`) and Virtual Push draws it: Clip mode's pages, Browse mode's categories and Mix mode's choices.
  - **Empty steps** were hard to tell from unlit pads, so they're now as bright as notes outside the key.

#### 5e: Generic MIDI controllers
- The definition schema and loader, the encoder decoders, and the included definitions (all marked unverified).
- Automatic detection, the Preferences → Controllers tab, how surfaces and MIDI Map work together, and the Surface Monitor.
- The learn wizard.

#### 5f: Push 2/3 driver
- MIDI, the LED palette sysex, and the display over libusb (fetched by CMake with a pinned hash, like the other dependencies).
- Detecting a missing WinUSB driver, and the udev rule for Linux.
- It's tested against reference bytes built from Ableton's spec. Hardware checks wait for a unit.

#### 5g: Several devices, MPE, and the quality pass
- Several surfaces at once, with shared or independent selection.
- MPE: Virtual Push's touch slide and timbre first, then Push 3 when a unit is available.
- **Quality pass:**
  - LED and display response measured at ≤ 1 frame;
  - pad-to-sound latency measured on the reference machine (a quiet-machine session, arranged with the user first);
  - the UI stress soak with Virtual Push open.

## Architecture — `src/control/`

```
 hardware / screen          driver                    device-independent             app
 ┌─────────────┐   ┌──────────────────────┐   ┌───────────────────────────┐   ┌──────────────────┐
 │ Push 2/3    │──▶│ PushDriver           │──▶│ SurfaceController         │──▶│ CommandRegistry   │
 │ Launchpad…  │──▶│ GenericMidiDriver    │   │  ├─ ModeStack (modes,      │   │ Model (undoable)  │
 │ Virtual Push│──▶│ VirtualPushDriver    │◀──│  │   overlays like Shift)  │◀──│ EngineStateFeed   │
 └─────────────┘   └──────────────────────┘   │  └─ outputs: LED frame,    │   └──────────────────┘
        ▲                    ▲                │     display frame          │
        │        fast path   │                └───────────────────────────┘
        └── pad notes ───────┴──▶ PadNoteMap (RtShared) ──▶ engine MIDI input (MIDI thread)
```

### `SurfaceCapabilities`: each driver describes its device
- **Grid:** rows × columns (0×0 if there is none), velocity sensitivity, and aftertouch type (none, channel, polyphonic, or MPE).
- **Encoders:** how many, absolute or relative, whether they're touch-sensitive, and their resolution. Also any extra encoders (master, tempo, swing/jog).
- **Buttons:** a list of **roles** (see below), each with its LED type.
- **Display:** a pixel display (width × height), a text display (lines × characters), or none.
- **LED colors:** full RGB, a palette (with its size), single color, or none.
- **Strip:** a touch strip or fader, if present.

### Roles: how buttons are named
- Modes never refer to hardware button numbers. They work with **roles**, for example:
  - transport: `play`, `record`, `shift`
  - mode buttons: `mode.session`, `mode.note`, `mode.device`, `mode.mix`, `mode.browse`, `mode.clip`
  - navigation: `nav.up/down/left/right`
  - numbered rows: `scene[n]`, `trackSelect[n]`, `displayUpper[n]`, `displayLower[n]`
  - actions: `octave.up/down`, `layout`, `scale`, `fixedLength`, `duplicate`, `delete`, `undo`, `quantize`, `doubleLoop`, `new`, `metronome`, `tapTempo`, `stopClip`, `mute`, `solo`, `stopAll`…
- Each driver maps its own hardware to these roles.
- **Graceful degradation:** if a device lacks a role, every mode still works through other routes. For example, a Shift+pad combination, or the action stays available on screen. Every mode declares the roles it needs and the ones that are optional, and tests check that each mode stays usable on a device with only a grid and no buttons.

### `SurfaceController` and `ModeStack` (device-independent, message thread)
- **One controller per connected device.** Several devices can work at once: Push + Virtual Push + Launchpad.
- Each controller can follow the **shared selection** (selected track, scene, and device) or keep its own. Default: follow.
- **The mode stack:**
  - a base mode (Session, Note, Device, Mix, Browse, or Clip)
  - temporary overlays on top (Shift, the Scale/Tuning menu, the Fixed Length menu, a delete-hold, and so on)
- **Outputs:**
  - **LED frames:** the desired state of every pad and button LED (color plus animation: solid, pulse, or blink, synced to the beat).
  - **Display frames:** what to draw on the device's display.
  - Drivers compare each frame with the previous one and send only the changes.
- **Inputs:**
  - commands go through `CommandRegistry`, the same path as the UI
  - edits go through the undoable model
  - live state is read from `EngineStateFeed`

### Fast path for pads: low-latency playing
- When a pad is pressed in Note or Drum mode, **the press does not wait for the message thread.**
- The controller publishes a **`PadNoteMap`** through `RtShared` (from M2):
  - which pad maps to which note, velocity curve, and target track
  - pressure handling
- The driver's MIDI input callback looks the pad up and sends the note **straight to the engine's MIDI input**, timestamped, from the MIDI thread.
- **The same fast path handles clip launches and scene launches.** M2's command FIFO becomes **one lock-free queue per producer thread** (UI, each MIDI input), so launching works even with quantization set to None.
- **Real-time rules:** the MIDI input threads now follow the RealtimeCheck rules too (no allocation, no locks).
- **Target:** a pad press is played in the next audio block, at the sample position matching its timestamp. The extra latency is no more than the audio buffer plus 1 ms.

## Modes (all adapt to the device's size)

### Session mode
- The grid shows clip slots: **columns = tracks, rows = scenes**. It uses the device's full size: 8×8 on Push, 8×12 or 16×16 on Virtual Push, and so on.
- Pad colors show the clip color and state:
  - playing → pulse
  - queued → blink
  - recording → red
- Scene buttons (or the last grid column, if there are no scene buttons) launch scenes.
- Navigation buttons move the view. Shift plus navigation moves by one full page.
- **Overview:** holding the Session button shows a zoomed-out map, with one pad per page of the set.
- Pressing an empty slot on an armed track starts recording. Holding Delete or Duplicate while pressing a pad deletes or duplicates that clip. The stopClip and mute/solo buttons change what the lower row of pads does.

### Note mode: layouts
- **Choosing a layout:** the layout follows the track's tuning automatically, and you can change it in the Scale/Tuning menu.
- **12-TET, In Key:**
  - like Push's In Key layout, with scales
  - row offset of a 4th, or set it yourself
  - the root note is lit
  - notes outside the scale are hidden
- **12-TET, Chromatic:** Push-style, with notes outside the scale shown dimmed.
- **Colundi / table layout** (the main design point):
  - **Which note each pad plays:** `index = origin + column × colStep + row × rowOffset`
    - Defaults: `colStep` = 1, `rowOffset` = 5.
    - Preset **Sequential**: `rowOffset` = number of columns. No note appears twice, so 64 of the 128 entries are visible on 8×8.
    - Presets for **Overlap 3/5/7**, or set any value yourself.
  - **Pad colors:**
    - **anchor notes** (exact multiples of 30 Hz) use the root color
    - notes in the track's **highlight set** (from M4) use the in-key color
    - other notes are dim
    - notes above the playable range (M1 high-frequency rule) are unlit, or red if pressed
    - a pressed pad lights green, together with every other pad that plays the same index
  - **Octave buttons:** they move `origin` by a row or by a full page, set in the menu.
  - **Display:** each pressed note's index, Hz, and label. The menu page shows the tuning, layout preset, origin, row offset, and highlight set.
  - This layout also works on any generic grid (Launchpad and similar) and on any Virtual Push size.
- **Drum layout:**
  - The grid is split into **zones** based on its size. On 8×8 these are:
    - a 4×4 pad area for the Drum Rack page
    - a 4×4 loop selector
    - a 32-step sequencer row
  - Larger grids get bigger pad areas and longer step rows. A "64 pads" option uses the whole grid for pads.
- **Melodic step sequencer:**
  - The upper area shows steps × notes and uses the active tuning. In Colundi, each row shows its index and Hz.
  - The lower area chooses the loop page.
  - Hold a step to edit its length, velocity, and nudge with the encoders.

### Device mode
- **Navigation:** the upper display buttons choose a device in the chain. You can step into and out of racks, and choose chains.
- **Encoders:** they show the pages from `ParameterPager` for the device's encoder count: 8 on Push; 16 or 32 on Virtual Push. For racks, the macros come first (any count, paged).
- **Display:** parameter names, values, and small value graphics. Touching an encoder shows a larger detail view (for example the curve for an EQ band).
- Lower buttons: device on/off, and page navigation.

### Mix mode
- Encoders control volume, pan, or send A/B/C… for the tracks in view. The upper buttons choose which.
- Lower buttons select the track. With Shift, or in the Mute/Solo/Arm sub-modes, they toggle those.
- Push's 8 track-select buttons show 8 tracks. A larger Virtual Push shows more tracks.

### Browse mode
- The browser's categories and items are shown on the display, and the encoders scroll the lists.
- Browsing **plays previews** (samples and presets), using the M4 preview bus. Pressing Load inserts or hot-swaps the item.

### Clip mode
- Edits the selected clip's start, end, loop, and launch settings with the encoders.
- **Fixed length** (record length), **double loop**, **quantize** (amount set in the menu), and **duplicate**.

### Global controls
- Play, record, and session-record/overdub.
- The tempo encoder; Shift+tempo nudges by 0.01 BPM.
- Metronome, tap tempo, undo, redo (with Shift), and new (an empty clip or scene).
- **Touch strip:** pitch bend by default (following M1's bend modes and range in cents or table steps). It can be set to mod wheel or to scrolling.
- **Pressure:** **polyphonic aftertouch** goes to each voice's pressure, and channel aftertouch is also supported.
  - The velocity curve and pressure threshold can be set (Push 2 pad sensitivity is set with sysex).
- **Push 3 MPE:** live per-note pitch slide, timbre (Y axis), and pressure. These go to M3's per-voice expression terms; this milestone adds the routing for per-note expression to the engine. Recording MPE into clips comes after the MVP.

## Drivers

### Ableton Push 2 — `src/control/push/`
Built from Ableton's published *Push 2 MIDI and Display Interface* (github.com/Ableton/push-interface). **Every constant below must be checked against that document during implementation.**
- **Finding the device:**
  - USB vendor and product ID (Push 2: VID `0x2982`, PID `0x1967`).
  - MIDI port names ("Ableton Push 2 Live Port" and "User Port").
  - A sysex identity request.
- **MIDI input:**
  - **Pads:** notes 36–99 (8×8, starting at the bottom left), with velocity and polyphonic aftertouch.
  - **Encoders:** relative encoders sending two's-complement CCs. Encoders also send touch notes.
  - **Strip and buttons:** the touch strip as pitch bend, and the buttons as CCs.
  - Handles the **Live port vs. User port** split, including the device's User mode for passthrough.
- **LEDs:**
  - Colors come from a palette. We load our own **RGB palette entries with sysex**, so the pad colors match our theme and clip colors.
  - The MIDI channel chooses the animation (blink or pulse, at a beat-synced rate).
  - A limit on outgoing messages per second prevents flooding the device's MIDI input. Changes are combined into one batch per display refresh.
  - Settings via sysex: pad velocity curve, aftertouch mode, LED and display brightness, and the touch strip configuration.
- **Display:**
  - 960×160 pixels, BGR565 color, sent over USB bulk transfer with **libusb**.
  - Each frame starts with the 16-byte header from the spec. Each line is padded to the stride the spec gives, and the pixel data is XOR-masked as specified.
  - A **dedicated USB thread** sends frames asynchronously, at up to 60 fps, and only when the content changed.
  - A watchdog resends the frame so the display doesn't time out.
- **Display rendering:**
  - The `DisplayFrame` is drawn into a `juce::Image` with our theme, and is then converted to the device format.
  - The display layout divides into **columns, one per encoder** (8 × 120 px on Push).
  - The drawing code is shared with Virtual Push's on-screen display and scales to its size.
- **Windows:**
  - USB access to the display needs the WinUSB driver (Ableton Live installs it). If it's missing, the app finds that out and links to instructions.
  - **Port sharing:** with classic Windows MIDI, a port can only be opened by one app at a time. If Live (or something else) holds the Push port, we show a clear message. Where the new Windows MIDI Services are available, we use them, since they allow multiple clients.
- **Linux:**
  - The display works through libusb directly (no driver swap). A **udev rule** shipped in `resources/linux/udev/` gives the user access to the device, and the app detects a missing rule and explains how to install it.
  - MIDI goes through the ALSA sequencer, which lets several apps share a port.

### Ableton Push 3 (controller mode) — same folder
- It builds on the Push 2 driver, plus:
  - **MPE pads** (per-pad pitch slide, Y axis, pressure)
  - the jog wheel / clickable encoder
  - its additional buttons, mapped to new roles
- **Risk:** it's unclear whether Ableton has published Push 3's differences.
  - **Plan:** use the Push 2 spec as the base, then capture the traffic between Live and Push 3 with a MIDI/USB monitor to find the differences.
  - **Requirement:** a Push 3 unit for testing.
  - Push 3 support is confirmed only when it's tested on real hardware.

### Generic MIDI controllers — `src/control/generic/`
- **Surface definition files** (`*.surface.json`, with a published JSON schema). Each one describes:
  - **Identity:** regular expressions for port names, an optional sysex identity response, and init/exit sysex messages (e.g. switching a Launchpad into programmer mode).
  - **Grid:** the note or CC number for each pad, given as a formula or a table, plus velocity and aftertouch sources.
  - **Encoders and faders:** CC numbers and the encoding: absolute, two's complement, binary offset, sign-magnitude, or 14-bit NRPN/high-low pairs.
  - **Buttons → roles**, and **LED feedback:** a velocity-to-color palette table, an RGB sysex template, or a simple on/off value.
  - **Display (optional):** a text sysex template with line and character counts. Pixel displays need a native driver.
- **Included definitions** (each written from the manufacturer's published programmer's reference):
  - Novation Launchpad X, Mini MK3, Pro MK3
  - Akai APC mini mk2, APC40 mkII, APC64 (if Akai's protocol for the APC64 isn't published, its definition starts from the information that is available and is completed using the device's captured messages)
  - a "keyboard + 8 knobs" template
  - a Maschine MK3 in MIDI mode, as a stopgap until M7

  **Each one is marked "verified" only after it's tested on the real device.** Until then they ship as "community/unverified".
- **Learn wizard** (Preferences → Controllers → "Add controller…"):
  1. Pick the MIDI ports.
  2. Press the top-left pad, then the bottom-right pad. The wizard works out the grid mapping, or you set it pad by pad.
  3. Turn each encoder; the wizard detects the encoding automatically from the values.
  4. Press each button and assign it a role from a list.
  5. Test the LEDs: the wizard tries common velocity-to-color schemes and asks which one looks right.
  6. Save, and optionally share the file.

  The result is a definition file that can be checked and edited.
- **Automatic detection:** when a MIDI device is connected, its ports are compared with the installed definitions, and the app offers to enable the match.
- **How it works with MIDI Map (M4):**
  - Ports used by a surface are "surface ports". Their pads and controls are handled by the surface, and MIDI Map applies on top for any controls not assigned.
  - Notes in Note mode can optionally pass through to tracks.

### Virtual Push — `src/control/virtual/`
- A dockable panel inside the M4 window, or its own window. It can go full screen, e.g. on a touchscreen second monitor.
- **Settings** (saved globally, plus per-set overrides):
  - grid from **4×4 to 16×16** (rows and columns set separately, e.g. 8×12)
  - **8, 16, or 32 encoders**
  - display on or off (its size follows the encoder count)
  - which button groups are shown, and pad size
- **Input:**
  - **Mouse:** velocity comes from the click position inside the pad (or is fixed, set by you). Right-click latches a pad. Encoders respond to dragging and the scroll wheel.
  - **Multi-touch** (touchscreens): several pads at once. On Linux, JUCE may not report multi-touch, so it starts mouse-only there. Adding XInput2 touch handling behind `platform::` is a follow-up. Pressure is used where the hardware reports it (pen or force touch). The finger's X and Y movement after the press act as **MPE-style per-note slide and timbre**.
  - The computer keyboard keeps working alongside it.
- **Role in the project:** it's the **reference driver**:
  - it supports every role and every mode
  - automated tests use it
  - new modes are built and checked on it before hardware

## Hardware choice: undecided (as of 2026-10-11)
The user hasn't bought a controller yet. The recommended options are a **used Ableton Push 2/3** or a **Novation Launchpad Pro MK3**. The APC64, APC40 MKII, and Maschine MK3 are also under consideration.

**Keeping every option open until then:**
- Every candidate's capabilities and roles are listed now, from public specs, so the modes are tested against each one's size using `FakeSurface` before any hardware arrives:
  - Push: 8×8 grid, 8 encoders, pixel display
  - Launchpad Pro MK3: 8×8 grid with polyphonic aftertouch, no encoders, no display
  - APC64: 8×8 grid, 8 touch strips, text display
  - APC40 MKII: 8×5 grid with no velocity, 16 encoders, 9 faders
  - Maschine MK3: 4×4 grid, 8 encoders, 2 displays
- Each candidate has a definition file (or, for Push, a native driver) in the M5 set. They are all marked unverified until tested.
- The `SurfaceDriver` interface lets any generic definition be **upgraded to a native driver** without changing the modes. That route is for a device with features JSON can't express, for example the Launchpad Pro MK3's fader banks on the grid and its pressure settings.

**Getting the purchased device working** (done together with the user):
1. Connect the device, open the Surface Monitor, and record every input (pads, buttons, encoders, pressure) to a capture file.
2. Compare the captures with the device's definition or driver and its published protocol. Fix any differences.
3. Turn the captures into **regression tests** (recorded input → expected commands, LEDs, and display), so later changes can't break the device.
4. Work through the hardware checklist (Tests section, item 7) and mark the device **verified**.
5. Decide whether it needs a native driver. If so, write a short addendum to this sub-plan.

## Tests
1. **Modes on fake devices:** a `FakeSurface` that takes any `SurfaceCapabilities`. For each mode, checks on grids of 8×8, 8×12, 16×16, and 4×4, and with 8, 16, and 32 encoders:
   - **Session:** the pad→slot mapping, navigation and page jumps, LED colors and animations for every clip state, and overview mode.
   - **Colundi layout:**
     - the index formula for each preset and for custom offsets
     - duplicate pads light together
     - anchor, highlight, and unplayable colors are correct
     - the origin stays within 0–127
   - **In Key / Chromatic:** each scale's pad mapping is checked against tables from Push's documented behavior.
   - **Drum:** the zones fit each grid size, and step-sequencer edits are turned into undoable model edits.
   - **Device:** the encoder pages equal `ParameterPager` for N, and macros (any count) come first.
   - **Mix / Browse / Clip:** encoders and buttons produce the expected commands and model edits.
   - **Graceful degradation:** with no buttons, only a grid, every mode can still be reached and used.
2. **Push driver encoding:**
   - **Input:** pad note ↔ grid position, and encoder decoding (two's complement, including fast turns).
   - **LEDs:** messages for each color and animation, palette sysex bytes, and the message-rate limit.
   - **Display:** frame bytes (header, line stride, XOR mask, BGR565 conversion) match **reference byte files** built from the spec.
3. **Generic definitions:**
   - Schema checks on every included file.
   - Decoding each encoder type, with edge values.
   - Init and exit sysex are sent at connect and disconnect.
   - A learn-wizard run with recorded MIDI input produces a valid definition that matches a known reference.
4. **Fast-path latency:**
   - **Timing:** timestamped pad events fed into the MIDI thread arrive in the engine block and at the sample offset their timestamp predicts, measured with M2's `OfflineRenderer`.
   - **No lag from the message thread:** a busy UI thread (simulated stalls of 500 ms) does **not** delay notes or launches.
   - **Real-time rules:** RealtimeCheck runs on the MIDI threads during a 10-minute pad-input fuzz, with no allocation or locks.
5. **Several devices at once:** Push + Virtual Push + a Launchpad following the shared selection stay in sync. With independent selection, they don't affect each other.
6. **Virtual Push rendering:** snapshot tests at several sizes, themes, and zoom levels. Simulated multi-touch produces correct per-note slide and pressure.
7. **Hardware checklist** (manual, with a written record kept for each release):
   - Push 2: every button, pad, encoder, touch, the strip, LEDs, and display. The display stays stable for 1 hour.
   - Push 3: the same, plus MPE.
   - Each generic definition: tested on real hardware before it's marked verified.
   - A **Surface Monitor** debug window shows every incoming and outgoing message and display frame, which helps when debugging.

## Definition of done
- [ ] With no hardware, Virtual Push at 8×8 and at 8×12 with 16 encoders can play a full session using every mode: launch, Colundi playing, drum sequencing, device editing, mixing, browsing.
- [ ] On Push 2: every mode works and the display shows our device pages. Colundi Note mode shows anchors and highlights, and plays the table frequencies.
- [ ] On Push 3 (if a unit is available): the same, plus MPE slides that bend smoothly along the Colundi table.
- [ ] At least one generic grid controller works in Session and Colundi Note mode from an included definition, and another works from a definition made with the learn wizard.
- [ ] Pad-to-sound latency adds no more than the audio buffer plus 1 ms, and a stalled UI doesn't affect it.
- [ ] All tests pass in CI. The hardware checklist is complete for the devices available.

## Risks
- **Push 3 protocol details** may not be published. We plan to capture and compare traffic, and we need a test unit.
- **Live running at the same time** holds the Push port on classic Windows MIDI. Users get a clear message; Windows MIDI Services allow sharing where available.
- **Generic controllers vary widely.** JSON definitions cover most grid and knob controllers. If some need complex behavior, we'd add an embedded scripting option (e.g. QuickJS) after M5, by the same pattern as the definitions.
- **Display drawing cost:** it renders only when something changed, on its own thread, capped at 60 fps. Its time counts toward the M4 UI performance budget.

## Interfaces kept for later milestones
- **M6:**
  - Hosted plugins appear in Device mode through `ParameterPager`.
  - MPE output to external synths reuses the per-note expression routing added here.
  - MIDI clock sync is shown on the device displays.
- **M7 Maschine MK3:** a native driver that implements `SurfaceCapabilities` (a 4×4 grid, 8 encoders, two displays, a smart strip). All modes run unchanged and arrange themselves for the 4×4 grid.
- **Post-MVP:**
  - MPE recording and editing in clips (the expression data model plus M4 lanes).
  - Automation and arrangement modes, added as new modes in the same `ModeStack`.
