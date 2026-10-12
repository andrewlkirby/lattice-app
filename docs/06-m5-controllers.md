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
