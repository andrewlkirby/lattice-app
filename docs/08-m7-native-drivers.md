# Sub-plan M7: Native Drivers for the Purchased Controller(s), and Finishing the MVP

Parent: `00-preliminary-plan.md`, milestone M7. This replaces the earlier "Maschine MK3 only" scope, as the user asked on 2026-10-11.
Depends on: M5 (the controller layer, modes, Virtual Push, generic definitions, Surface Monitor, steps for a new device) and M6 (timed MIDI out, External Instrument, audio devices).

## Goal
1. Give **whichever controller(s) the user buys** a native driver, built and checked **with the user and the real device**. A native driver supports everything the device can do, beyond what a JSON definition can express.
2. **Finish the MVP:** an end-to-end acceptance run, a soak test, installer, licensing review, and a final performance sign-off.

**Candidates** (decision pending, see M5 "Hardware choice"):
- Recommended: **used Ableton Push 2/3** or **Novation Launchpad Pro MK3**
- Also under consideration: **Akai APC64**, **Akai APC40 MKII**, **NI Maschine MK3**

Nothing in M7 depends on which device is chosen. The framework, the process, and the notes for each candidate below cover all of them.

## When a device needs a native driver
A generic definition (M5) is enough when the device is plain MIDI: notes and CCs in, palette LEDs out. A native driver is written when the device has **any** of the following:
- a pixel display, or a display protocol more complex than a text sysex template
- input over USB HID or a vendor USB protocol (not MIDI)
- a handshake, mode switch, or special state handling that JSON can't express
- features that need two-way setup: grid faders, LED-ring modes, touch-strip LED bars, pressure configuration
- performance needs: batched RGB updates, high-rate input

The native driver implements the same `SurfaceDriver` interface, so **modes, tests, and the Virtual Push stay unchanged**.

## Native driver framework — `src/control/native/`
Every native driver must provide:
1. **Finding the device:**
   - USB vendor and product ID and/or MIDI port names, plus the serial number. With the serial number, **two of the same device** can work at once, each with its own settings.
   - Firmware version detection, checked against a **compatibility table** (tested firmware versions). If the firmware is newer and untested, the app warns but still tries to run.
2. **Start-up and shutdown:**
   - Puts the device into the right mode on connect.
   - **Puts the device back into its normal standalone state** when the app quits, the device is disconnected, the computer sleeps, or the app **crashes** (a crash handler does the cleanup).
   - The device must never be left in a strange state.
3. **Hot-plugging:** connecting and disconnecting while the app is running, and reconnecting after sleep or resume. The full LED and display state is restored after reconnecting.
4. **Input:** decoding messages into the M5 events (pads, buttons, encoders, strips, pressure). Pad notes use the **fast path** (`PadNoteMap` on the MIDI or HID thread, no locks, no allocation).
5. **Output:**
   - LED changes are compared with the previous state and sent in batches, with a limit on messages per second.
   - Displays are redrawn only when content changes, at a capped frame rate, on their own thread.
   - **Budgets:** each driver documents its output bandwidth and frame rate, and both are measured.
6. **Capabilities:** an accurate `SurfaceCapabilities` and role map, including any **new roles** the device adds (e.g. `faders.virtual`, `strip[n]`, `jog`). Modes only use new roles where they're available.
7. **Settings:** a page in Preferences → Controllers for things like the velocity curve, pressure mode, brightness, and options specific to the device.
8. **Debugging:** every message in and out is visible in the Surface Monitor. Captures can be recorded and replayed.
9. **Tests:** the device's real recorded captures are replayed as regression tests (recorded input → expected commands, LEDs, display output).

## Steps for the purchased device (done together with the user)
This continues the M5 "Hardware choice" steps:
1. **Capture:** connect the device and record every control with the Surface Monitor. For devices that don't use MIDI, use USB capture tools (USBPcap + Wireshark on Windows; `usbmon` + Wireshark on Linux) while the maker's own software runs.
2. **Write down the protocol** in `docs/protocols/<device>.md`: message tables, handshakes, LED and display formats. Each entry is marked "from the official docs" or "found from captures".
3. **Write a driver addendum** `docs/08x-driver-<device>.md` from the template at the end of this document, listing the device's capabilities, roles, and any new roles, plus how modes are arranged on it and its known limits.
4. **Build the native driver** within the framework above, test it with the captures, then try it live together.
5. **Tune how it feels on the actual device:**
   - velocity and pressure curves
   - LED colors adjusted for the device's LEDs (each device shows colors differently)
   - display layout
   - **Colundi layout defaults suited to the device's grid**
6. **Mark it verified:** complete the hardware checklist, add the tested firmware version to the compatibility table, and mark the device verified in the docs.

## Notes for each candidate
All specs below come from public information and **have to be checked against the real device and its documentation**.

### Ableton Push 2 / Push 3: native driver already exists (M5)
- In M7: run the steps above to **verify** with real hardware, and capture Push 3's differences from Push 2 (MPE, jog wheel, extra buttons).
- Build a pad sensitivity calibration screen.
- Refine the display pages on the real screen (the colors and contrast of the real display differ from the Virtual Push).
- If it's a used unit, check its firmware version and update it through Ableton's tools if needed (and record the version).

### Novation Launchpad Pro MK3: native driver recommended
- Novation publishes a **Programmer's Reference Guide**. The protocol is fully documented, so the driver is low risk.
- **What the native driver adds beyond the JSON definition:**
  - **DAW mode** on its separate DAW ports, with Novation's session and **fader layouts**. The grid columns become **virtual faders**, set up with sysex and with value feedback.
    - These fill the `faders.virtual` role: Mix and Device modes use 8 grid faders in place of encoders.
    - This makes up for the Launchpad having no knobs.
  - **Full RGB LED colors through sysex**, sent in batches for fast updates: exact clip colors and the Colundi anchor and highlight colors.
  - **Pressure setup:** polyphonic or channel aftertouch, and the pressure threshold, set from our Preferences.
  - Handling the mode buttons and the device's own sequencer modes, so it switches cleanly between our modes and its standalone features.
  - **Its MIDI DIN outputs** appear as normal MIDI output ports, so the Launchpad can also act as the MIDI interface for M6 hardware retuning.
- **Modes on it:**
  - Session uses 8×8.
  - Colundi Note uses 8×8 with polyphonic pressure. The Sequential preset is probably the best default here; to be confirmed when playing it together.
  - Device and Mix use the grid faders.
  - The Virtual Push on screen gives the display and encoders when wanted.

### Akai APC64: native driver if its protocol is accessible
- 8×8 RGB pads (velocity and pressure to be confirmed), **8 touch strips with LED bars**, a small display, its own sequencer, and **CV/gate outputs**.
- **Main risk:** whether Akai publishes its protocol. If not, we use the capture steps. If the strips, LEDs, and display can't be fully controlled, it falls back to a generic definition plus the Virtual Push.
- **What the native driver adds:**
  - LED-bar feedback on the strips (strips as encoders, using the `strip[n]` role)
  - text on the display
  - mode handling
- **CV outputs (to investigate):** whether the computer can control them, and with what pitch resolution. If it's fine enough, **Colundi pitches could go to modular synths** as CV. That would be an addition to M6's retuning methods ("CV pitch"), and the Tuning Verifier would check it.

### Akai APC40 MKII: native driver for full mixing feedback
- Akai's protocol document covers the **mode-switch sysex** (Generic / Ableton Live / Alternate Ableton Live modes) and the **LED-ring styles** of the knobs.
- **What the native driver adds:**
  - switching modes at start-up
  - knob LED rings showing each parameter's type (single dot, volume, pan)
  - faders with soft takeover
  - **The crossfader** is assignable (any macro or parameter), because our mixer has no crossfader for now.
- **Limits:**
  - The clip buttons have no velocity, so Note mode uses a fixed or settable velocity, and pressure is unavailable.
  - Best used **together with a pad controller or the Virtual Push**. M5 already supports several devices following the shared selection.

### NI Maschine MK3: hardest, native driver with reverse-engineering
- 4×4 pads with pressure, 8 touch-sensitive encoders, **2 color displays**, a smart strip, a 4-direction push encoder, and a **built-in audio interface**.
- **The protocol is not public:**
  - Input and LEDs go over USB HID.
  - The displays use a vendor USB bulk protocol.
  - Work comes from capturing USB traffic, plus existing community reverse-engineering projects (check their licenses before reusing any code; otherwise write our own from the captures).
- **Windows issues:**
  - NI's background services may hold the device; the app explains how to stop them.
  - **Display access may need a WinUSB driver installed for that interface** (e.g. with Zadig). This can break NI's own software on that machine. The user must agree to this, and the app documents how to undo it.
  - HID input through hidapi may work without replacing the driver. That's checked first.
- **Linux is likely easier:** NI software doesn't run there, so nothing competes for the device. Access goes through hidraw/libusb with a udev rule, and no driver swap is needed.
- **Legal:** reverse-engineering for compatibility is generally allowed in many places, but check this for the user's situation before shipping. This is a note, not legal advice.
- **Modes on a 4×4 grid** (no changes to the modes, thanks to M5's adaptive design):
  - Session: pages of 4 tracks × 4 scenes.
  - Colundi Note: 16 pads, with the Sequential layout moving by pages of 16.
  - Drum: maps 1:1 to a Drum Rack page.
  - Device: 8 encoders, with 2 displays of parameter pages.
- **Audio:** its audio interface works as a normal audio device with no driver work from us.
- **Firmware updates** may change the protocol, so the compatibility table and the capture-based regression tests matter most here.
- Expect this to be the **largest** driver effort of all the candidates.

## Finishing the MVP — `docs/release/`
Once the purchased device is verified, these complete the MVP:
1. **End-to-end acceptance test**, run with the user on the reference machine:
   - Start a new set and choose the Colundi tuning.
   - Build a set with the controller alone:
     - Drum Rack beats
     - Colundi synth parts played on pads
     - a plugin synth retuned through MTS-ESP
     - a hardware synth retuned with MPE and checked with the Tuning Verifier
   - Mix with returns and sidechain, then launch scenes in a live-style performance.
   - Save, reopen, and recover from a forced crash.
   - **Repeat it with no controller connected**, using the mouse, keyboard, and Virtual Push only.
2. **Soak test:** run an 8-hour playback and editing session (scripted UI and controller input) at a 64-sample buffer, with zero dropouts, no memory growth, and no stuck notes.
3. **Final performance sign-off:** every `lattice_bench` budget (M3, M4, M6) and the latency targets (M5 fast path, M6 MIDI jitter) are met on the reference machine.
4. **Set format check:** a collection of test sets saved by every milestone build loads correctly through the format migration hooks (from M2).
5. **Installer and distribution:**
   - A Windows installer (WiX or Inno Setup). It installs the app, factory content, the plugin scanner, and optional components (the MTS-ESP library notice, and the WinUSB note for controllers with displays).
   - **Linux packages:** a `.deb` (for Ubuntu 24.04 and later) and an **AppImage**. Both install the udev rules (the AppImage through a one-time helper prompt) and a desktop entry. Flatpak isn't used, because its sandbox gets in the way of plugin folders, USB access, and real-time priority.
   - Code signing (a certificate is needed; it costs money).
   - **Crash dumps stay on the computer**: minidumps on Windows, and a Breakpad/Crashpad-style local handler on Linux. Nothing is uploaded, which fits the offline rule.
6. **Licensing review before any release:**
   - JUCE: AGPLv3 or a commercial license
   - ASIO SDK terms
   - MTS-ESP SDK terms
   - CLAP: MIT
   - SQLite: public domain
   - any code from community projects
   - how we describe compatibility with Ableton, Novation, Akai, or NI devices (names used only to describe compatibility; no logos or copied assets)
7. **User docs:** an in-app help section for each view and mode, a Colundi guide (tunings, layouts, retuning external gear), and setup pages for each controller.

## Driver addendum template (`docs/08x-driver-<device>.md`)
```
# Driver: <device name>
Firmware tested: <versions>          Status: unverified | verified (date)
## Connection        USB VID/PID, MIDI ports, handshakes, mode switching, cleanup on exit
## Capabilities      grid, encoders/strips/faders, display, LED type, pressure type
## Role map          hardware control → role (and any new roles)
## Modes on this device   how each mode is arranged; Colundi layout defaults
## Protocol notes    link to docs/protocols/<device>.md; parts from official docs vs found from captures
## Settings          what's in Preferences for this device
## Limits / known issues
## Test captures     list of recorded captures used as regression tests
```

## Tests
- **Framework:**
  - Start-up and shutdown and the cleanup on exit are checked by simulated disconnects, sleep and resume, and a forced crash: the device ends up in its normal standalone state.
  - Two of the same device work at once.
  - The firmware compatibility table logic warns correctly.
- **For each driver:**
  - The recorded captures replay to the expected commands, LEDs, and display output.
  - LED and display bandwidth stay within the documented budget.
  - The fast-path latency test from M5 passes with the device's own input format.
- **Modes on the device's real capabilities:**
  - The M5 `FakeSurface` tests run again with the device's exact capability set, including any new roles: grid faders, strips, a 4×4 grid.
  - Graceful degradation is checked for roles the device lacks.
- **MVP:** the acceptance test, the soak test, the performance sign-off, and the set format check above.

## Definition of done
- [ ] The controller(s) the user bought have a native driver (or a verified definition, where native adds nothing), a driver addendum, a protocol document, and capture-based regression tests.
- [ ] The hardware checklist is complete, and the device is marked **verified** with its firmware version recorded.
- [ ] The device returns to its normal standalone state after every kind of exit, including a crash.
- [ ] The MVP acceptance test passes both with the controller and without it.
- [ ] The 8-hour soak test passes, and every performance and latency target is met.
- [ ] The installer works on a clean Windows machine, and the licensing review is complete.
- [ ] The `.deb` and the AppImage install and launch in a clean Ubuntu 24.04 container (the GUI under Xvfb). If a Linux machine is available, the acceptance test is also run there with real audio and the user's controller.

## After the MVP (not in M7)
- Drivers for any additional controllers follow the same steps and template.
- Things deferred so far, each needing its own sub-plan:
  - arrangement view, audio tracks and recording, warping
  - automation, MPE recording and editing, groove
  - note probability and velocity ranges
  - Ableton Link, CV output
  - routing MIDI between tracks, the Repeat launch mode, count-in, Push 1
  - scripting in controller definition files
  - macOS release hardening
