# Lattice Docs

Planning documents for the Ableton-Live-like DAW with Push-style and generic MIDI controller support and Colundi tuning.

| Doc | Covers | Status |
|-----|--------|--------|
| [00-preliminary-plan.md](00-preliminary-plan.md) | Overall scope, architecture, milestones, risks | Approved |
| [01-m0-skeleton.md](01-m0-skeleton.md) | M0: CMake/JUCE skeleton, audio device, test tone, CI, real-time checks | Done (2026-10-11): all definition-of-done items met |
| [02-m1-tuning-core.md](02-m1-tuning-core.md) | M1: Tuning tables, Colundi/Scala parsers, per-track tuning, test synth | Done (2026-10-11): all definition-of-done items met |
| [03-m2-engine-session.md](03-m2-engine-session.md) | M2: Project model, real-time engine, transport, mixer, clip launching, recording | Done (2026-10-11): all definition-of-done items met |
| [04-m3-devices.md](04-m3-devices.md) | M3: Device framework, Synth/Sampler/Drum Rack, MIDI and audio effects, racks and macros, returns/sends/sidechain/delay compensation, presets | Draft |
| [05-m4-ui.md](05-m4-ui.md) | M4: Live-style UI (Session, mixer, groups, Clip view with Colundi piano roll, Device panels, browser and library, tuning tools, preferences, MIDI/Key Map) | Draft |
| [06-m5-controllers.md](06-m5-controllers.md) | M5: Controller layer (capabilities, roles, modes), Virtual Push, Push 2/3 drivers, generic MIDI controller definitions and learn wizard, low-latency pad path | Draft |
| [07-m6-plugins-external.md](07-m6-plugins-external.md) | M6: VST3/CLAP/AU hosting, scanning, crash isolation, MTS-ESP and tuning exports, retuning hardware (MPE/MTS), External Instrument, MIDI clock, Tuning Verifier | Draft |
| [08-m7-native-drivers.md](08-m7-native-drivers.md) | M7: Native drivers for whichever controller is purchased (framework, steps, notes for each candidate, addendum template), finishing the MVP | Draft |
| [09-performance-charter.md](09-performance-charter.md) | All milestones: latency, loading, and responsiveness targets; measurement; gates; coding and build rules | Draft |

**Applies to every milestone:** [09-performance-charter.md](09-performance-charter.md) — the targets for playing feel (latency) and load time, how they're measured, the gates every change goes through, and the coding and build rules.

**Rule for every plan:** the app must be fully usable with no hardware connected. Every controller feature has a mouse/keyboard equivalent. See "Design principle" in the preliminary plan.

**Priorities:** quality and performance before schedule; no hard-coded hardware sizes.

**Platforms:** Windows is the main platform. Linux is second-tier and tested on every commit through Docker and CI (`scripts/linux-test.ps1`). macOS comes later. All platform-specific code lives in `src/platform/`. See "Platforms" in the preliminary plan.

All MVP sub-plans are written. Next:
- When the controller is purchased: add `docs/protocols/<device>.md` and `08x-driver-<device>.md` (template in M7).
- Post-MVP features get their own sub-plans (list at the end of M7).