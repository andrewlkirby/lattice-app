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

## Starting point (updated 2026-11-07, after M5)

### What M2–M5 leave to build on
- **Used as they are:**
  - `StudioDevice`, a `juce::AudioProcessor`. Its `processInChain` gives a plugin wrapper the on/off crossfade and the latency-aligned dry path for free.
  - Parameter groups and `customPages`, which feed `ParameterPager`. So Device mode on Virtual Push and Push pages a plugin with no new controller code.
  - `ParameterSpec`'s text, both ways (5g's fix). MIDI Map's targets (`ui/mapping/Targets`) and the macros, which reach any `DeviceParameter`.
  - Delay compensation:
    - `DeviceChain::latencySamples`;
    - the delays of the sends and returns in the snapshot;
    - the sidechain routes (`SidechainRoute`).
  - The parallel `WorkerPool`, on real-time threads in Windows' Pro Audio (MMCSS) class. Plugins run there.
  - **MIDI:**
    - the lock-free live MIDI queue, with each event placed at its timestamp's sample (5b);
    - `TimedMidi::perNote` with `PerNoteExpression` (5g): the per-note expression that now also goes out to plugins and hardware.
  - **The library:** its SQLite index (`items`, full-text search, collections), and `JobPool` for background work. Scan results and the browser's Plugins category go there.
  - **Tunings:**
    - `TuningRegistry` and `TrackTuning`;
    - M1's parsers (Colundi text, Scala), which read the exports back in the tests;
    - the M4 tuning editor, which gets the exports.
  - **Preferences → MIDI:** the inputs' rows, whose Sync switch is waiting for 6e.
  - **Measuring:** `AudioCallbackStats`, the `lattice_bench` baselines, and `scripts\perf-check.ps1 -Soak`, for per-device CPU and long runs.
- **What has to change:**
  - **Devices are created inside the snapshot build.** `SnapshotBuilder::buildChain` calls `DeviceRegistry::create` synchronously on the message thread.
    - That suits built-in devices, which are built in microseconds. A plugin can take seconds, and a set with 30 of them must not freeze the window.
    - So plugins are instantiated ahead of the build by a `PluginInstancePool`. The build puts in a **Loading** placeholder, which is silent for an instrument and passes audio through for an effect. When the instance is ready, the next build swaps it in with the usual crossfade.
    - **Loading a set** starts every plugin first, in parallel wherever the format and plugin allow it (many VST3s must be created on the message thread). This is measured against the charter's load-time targets.
  - **A device's latency is fixed between prepares** (`setLatency` only works inside `prepare`). A plugin can report a new latency at any time, so a change has to recompute the snapshot's delays on the message thread without preparing the device again, as 6b describes.
  - **Parameter values only flow one way,** from the document to the engine. A plugin's own window changes them too, from any thread.
    - Its changes reach the message thread through a lock-free queue. The plugin's gesture begin and end marks each undo step.
    - **Only some parameters are kept in the document as values:** the ones chosen in Configure, mapped in MIDI Map, or on a macro. The rest live in the plugin's state, as in Live. So a plugin with 2,000 parameters doesn't add 2,000 nodes to the set.
  - **A device's parameter list is fixed when it's built.** A plugin's is only known once it's instantiated, and some plugins rename or regroup their parameters later. The wrapper refreshes its names and groups on the message thread, and its pages follow.
  - **JUCE's plugin hosting is off:** `CMakeLists.txt` sets no `JUCE_PLUGINHOST_*`.
    - 6a turns on VST3, plus AU on macOS and LV2 on Linux.
    - It also adds the test plugins (`tests/fixtures/plugins/`, built with `juce_add_plugin`). Every phase uses them. The Linux run in Docker hosts them with no window.
  - **JUCE 8.0.15 can't host CLAP.** At 6a's start, check whether a newer JUCE can. If not, we write our own CLAP host from the CLAP headers, as planned, behind the same format interface.
  - **MIDI only comes in.** Tracks can't send MIDI out yet; controllers send through `SurfaceManager`'s own ports on the message thread.
    - 6d adds the engine's timed MIDI out.
    - Ports get one owner, shared with the controllers, so a port can't be opened twice. A port a controller uses isn't offered as a track output.
  - **The transport only follows its own tempo.** 6e adds following an external clock.
  - **Tracks can't pass live audio input through yet.** The External Instrument needs it: monitoring the input, with delay compensation (6d).
  - **Test isolation:** in 5g, two tests failed when run in parallel (`-j 8`). They share the user library's files across test processes. 6a fixes that first, since the scanner's tests add more shared files (a scan database for each test).
- **Hardware here: none yet.** There's no external synth and no audio interface (as of 2026-11-07). Until there is:
  - a test plugin that decodes MPE bend and MTS sysex into pitch stands in for a hardware synth, and so does a free synth listening on a loopback MIDI port (see "Hands-on checks");
  - MIDI timing is measured on a loopback port. Windows MIDI Services' loopback is used if it's installed. Otherwise a virtual-port driver is needed; that's the user's choice to install;
  - the External Instrument's latency measurement is tested on a simulated delay, and later with a cable from an output to an input.

  These checks are listed for a second run when hardware arrives, as Push 3's MPE was.

### Order of work
- **Changed from the first draft:**
  - Crash isolation moves to the end, as its Risks entry allowed, because it's the most complex part.
  - Tuning for plugins moves up right after hosting, because it's the milestone's main Colundi result.
  - A quality pass is added at the end.
- **Phases:** 6a → 6b → 6c → 6d → 6e → 6f → 6g.
  - **6a:** scanning and the plugin library;
  - **6b:** hosting plugins as devices;
  - **6c:** tuning for plugins (MTS-ESP and exports);
  - **6d:** MIDI out, retuning hardware, and the External Instrument;
  - **6e:** clock sync and the Tuning Verifier;
  - **6f:** crash isolation;
  - **6g:** the quality pass.

  6c's exports (`.tun`, `.scl`/`.kbm`, MTS sysex) depend on nothing else, so they can fill any gap. 6d's hardware half needs only the engine; plugins that send MIDI out need 6b.
- **Each phase** ends as M5's did:
  - its tests pass on Windows and on Linux, with warnings as errors;
  - the performance targets it touches are checked;
  - the user does a short hands-on check.

  None of the checks need hardware.

#### Hands-on checks
These use free plugins, which the user installs before the phase that needs them:
- Surge XT: VST3 and CLAP, an MTS-ESP client, MPE, and a standalone app;
- ODDSound's MTS-ESP Mini, which installs the system library MTS-ESP needs.

The checks:
- **6a:** scan the plugin folders; find Surge XT in the browser. The test plugins that crash or hang end up on the blocklist.
- **6b:** check these with Surge XT:
  - it plays on a track;
  - its pages show on Virtual Push and Configure works;
  - its window opens;
  - it comes back as it was after save and reload;
  - it survives being removed and reinstalled as a placeholder.
- **6c:** Surge XT plays Colundi through MTS-ESP. Two tracks with different tunings each play their own.
- **6d:**
  - **Hardware stand-in:** Surge XT's standalone app, listening on a loopback port, plays a Colundi clip retuned with MPE bend.
  - **External Instrument:** its audio return is tested on the laptop's input.
- **6e:** the Tuning Verifier reports Surge XT (over MTS-ESP) and a built-in Synth within tolerance.
- **6f:** the crash test plugin in a sandbox; the set keeps playing.
- **6g:** none. Its numbers come from a quiet-machine session, which is arranged with the user first.

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
- **Preferences → Plugins:** folders, rescan, the blocklist, the default isolation mode (6f), and the preferred format when a plugin exists in several formats.

### As built (6a, 2026-11-07)
- **Where things are:**
  - `src/hosting/`: `PluginFormats` (the formats hosted here, in their default order) and `PluginLibrary` (the scan in the background, and the browser's rows);
  - `src/hosting/clap/ClapFormat`: CLAP as a `juce::AudioPluginFormat`;
  - `src/hosting/scan/`: `ScanProtocol` (what the app and the scanner say to each other), `PluginScanner` (runs the scanner and watches it), and `ScannerMain` (`LatticePluginScanner`).
  - The scanner is built beside the app and copied next to `Lattice.exe` after each build.
- **Formats:**
  - **VST3** everywhere, **LV2** on Linux, **AU** on macOS, through JUCE (`JUCE_PLUGINHOST_*` set in `lattice_juce`).
  - **CLAP:** JUCE 8.0.15 can't host it, and JUCE hasn't announced plans to. So it's Lattice's own format, from the CLAP 1.2.9 headers (MIT), behind JUCE's interface.
    - Scanning a file reads each plugin's descriptor. It also makes a short-lived instance, for the audio and note ports the descriptor doesn't give, as JUCE's VST3 scan does.
    - A file may hold several plugins. Each is identified by a hash (FNV-1a) of its CLAP id, which is the same on every machine.
    - Loading a CLAP as a device comes in 6b. Until then the format reports that it can't.
  - **Default order** when a plugin comes in several formats: CLAP, then VST3, then the OS's own. Preferences can choose another first.
  - **Default folders:**
    - CLAP: the spec's folders plus `CLAP_PATH`;
    - VST3 and LV2: JUCE's.

    Folders added in Preferences are searched for every format.
- **The scanner** (`LatticePluginScanner`):
  - **Jobs in, results out:** the app writes the files to scan to a jobs file and starts the scanner from a given line. The scanner appends a `begin`, its `plugin` lines and an `end` for each job to a results file, flushing each line to the OS before going on.
    - A file, not the scanner's output, since plugins print to stdout.
    - Each field is escaped onto its line, and the app reads only whole lines, so a line cut off by a crash is skipped.
  - **Each job is one message on the message thread,** so a plugin's own messages are handled in between, as in a host.
  - **No dialogs:** Windows Error Reporting's and the C runtime's abort and assert dialogs are off. A crash writes a `crash` line saying how ("access violation", "segmentation fault") before the process ends: on Windows from the unhandled-exception filter, on Linux from a signal handler that only calls `write()`.
  - **The app's side** (`PluginScanner::run`, on a background thread): it polls the results file every 10 ms.
    - **A crash:** a `begin` with no `end` before the process ends. The job is marked crashed, with the reason from the `crash` line or the exit code.
    - **A hang:** no `end` within the time limit (30 s by default). The process is killed and the job marked hung.
    - Either way a new scanner starts from the next job: one bad plugin costs one restart.
    - Two starts in a row that scan nothing stop the run, with why.
    - The scan's files are deleted when it ends.
- **The plugin library** (`PluginLibrary`, its own thread at low priority):
  - **What's on the disk:** each format's folders are searched, and each file is stamped with its size and modification time. For a bundle, that's its files outside `Resources`, so a changed binary is noticed and new artwork isn't. LV2's identifiers that aren't files are stamped 0, and are scanned once while they're listed.
  - **What's scanned:**
    - a new file, or one whose stamp changed;
    - with Rescan All, every file that isn't blocked;
    - with Retry, the files asked for, blocked or not.

    Files gone from the disk are forgotten.
  - **Blocked:** a file that crashed or hung the scanner, kept with the reason and not scanned again until it changes or a retry is asked for. A file the format can't read is kept as failed, by the same rule.
  - **Where results go:** into the M4 library database, in two tables of their own:
    - `plugin_files`: each file's stamp, status and reason;
    - `plugins`: each plugin's description, as JUCE's XML with Lattice's attributes.

    They're created alongside the browser's and kept when the browser's schema changes, since rescanning every plugin takes minutes. As results arrive, the rows are written and the browser's rows are remade, at most once a second.
  - **Starting:** the app starts a scan once it's ready (after the startup log's "ready"; never under `--soak` or `--quit-when-ready`), so start-up isn't slowed. The tests' shells never scan the machine's plugins.
- **The browser:**
  - **Plugins** is a category after MIDI Effects, as in Live. It lists folders that open on their plugins, grouped by vendor (the default), format, or type (Instruments, Audio Effects, MIDI Effects).
  - A plugin's row reads "Vendor · Format · Type".
  - **One row for each plugin:** the same name and vendor in several formats is one plugin, shown in the preferred format. Grouped by format, though, each format's folder lists all of its own plugins, since that's where a format is chosen.
  - **A row's name comes first:** the detail gets what the name leaves (at most half the row), so a narrow browser cuts the detail, not the name. Text widths are cached for each of the two fonts.
  - Plugins are found by search ("plugin", the vendor, the format) and can go in collections, keyed by the description's identifier, so they stay through rescans.
  - **Inserting a plugin** (double-click, drag, Insert) comes with 6b. Its row offers nothing yet.
- **Preferences → Plugins:**
  - Folders: the standard folders on or off, and more folders added or removed.
  - The preferred format, and the grouping.
  - Scan and Rescan All.
  - The status: scanning so many of so many files, or how many plugins (a plugin in several formats counted once) and how many in each format, "2 plugins (CLAP 2, VST3 2)", with why the last scan stopped if it did.
  - Blocked and failed files, each with its format and reason, and Retry or Retry All.

  Find finds each setting. The time limit has no setting yet: 30 s, as planned.
- **Known from the scan, not yet for every format:**
  - **CLAP** gives MIDI in and out and MPE support at scan time (from its note ports).
  - **VST3 and LV2** give them only from an instance's buses and controller. They're stored as unknown (-1), to be filled in when a plugin is first loaded (6b).
- **Tests:**
  - **The test plugins** (`tests/fixtures/plugins/`, built only into the build folder, never into a system folder):
    - "Lattice Test Plugin", a JUCE-built sine synth: VST3, and LV2 on Linux;
    - "Lattice Test Clap", two plugins in one file: a synth with the same name and vendor as the JUCE one, so the two are one plugin in two formats, and a note effect;
    - CLAP and VST3 files that crash or hang at the host's first call.
  - **`tests/hosting/PluginScanTests.cpp`:**
    - the protocol's lines and escaping (damaged lines skipped);
    - the CLAP file's descriptors, features, ports and MPE;
    - CLAP files found in subfolders;
    - one scanner run over nine jobs. Crashes and hangs (CLAP and VST3) get the right outcomes and reasons, the good files after them are still found, and it takes five processes. A file that isn't a plugin, and a format that isn't hosted, fail with why;
    - a scanner that's missing;
    - the plugin library end to end:
      - what's found;
      - the blocked file, with its reason;
      - the browser's groups and rows, the same plugin shown once and as the CLAP;
      - search;
      - an unchanged rescan scanning nothing, a changed file alone, Rescan All leaving out what's blocked while Retry doesn't;
      - files gone being forgotten;
      - the preferred format and the grouping;
      - built-ins being reset leaving the plugins;
    - a bundle's stamp;
    - LV2 on Linux.
  - **Browser and Preferences tests:**
    - the Plugins category's groups opening, a plugin not inserted yet, search and Favourites;
    - the Plugins tab saving the grouping, the format and the folders, and giving them to the scan;
    - every new setting found by Find.
  - **Test isolation (from 5g):** a test process's user-data folder, and every test's temporary folder and settings file, now has a name unique to it (a UUID). A name that's merely free when asked for could be taken by another process before it's made, so tests run in parallel (`ctest -j`) had shared files.
  - **A test that depended on memory addresses:** the Tab and Shift+Tab test took "the first knob in the Device view" from the control registry, a hash map keyed by pointer. So it could pick the device's last knob, from which Tab leaves the view and Shift+Tab rightly switches Clip/Device. The new code moved the addresses, and it failed once on each OS. It now takes the top-left knob.
- **Checks (2026-11-07):**
  - **Windows:** all 540 fast tests pass in one process, and with `ctest -j 8`, where the 5g failures are gone.
  - **Linux:** all 539 pass with warnings as errors, LV2 included.
  - The scan tests take about 5 s, mostly the two hangs at a 1.5 s limit.
- **Walkthrough with real plugins (2026-11-07):** `tests/hosting/PluginWalkthroughTests.cpp` (hidden, `[plugins]`) scans the folder in `LATTICE_PLUGIN_FOLDER` through a shell's own plugin library, with the crashing test plugins added. It then works the browser and the Plugins tab as a person would, saving an image at each step to `LATTICE_WALKTHROUGH_DIR`. The scanner is now copied beside the tests too, where a shell looks for it.
  - **Run over Surge XT 1.3.4** (its plugins-only zip, unpacked in a temporary folder: nothing installed):
    - 6 files in 2.0 s (0.3 s again with the files cached);
    - all four plugins found, with the right names, vendor and version: Surge XT and Surge XT Effects in CLAP and VST3;
    - the CLAP Surge XT read as an instrument taking MPE, with 2 inputs and 6 outputs (its scenes' outputs), and the VST3 with JUCE's default stereo layout;
    - both crashing test plugins blocked with "access violation";
    - a rescan with nothing changed scanned nothing, in 0.01 s.
  - **Found by it, and fixed:**
    - **Grouping by format showed only the preferred format's folder,** since each plugin was still shown once. Each format's folder now lists all its plugins.
    - **The status said "4 plugins"** where the browser showed 2: it now counts as the browser does, and gives each format's count.
    - **In the browser at its default width, "Surge XT Effects" was cut to "Surge XT Ef…"** while the detail kept half the row. The name now comes first.
    - **The new browser test left its plugin in the test process's library,** in the Favourites, where a later test found it. It now cleans up after itself.
    - **A "Bad Image" box from Windows.** The CLAP test reads a file that isn't a library in the test process itself, not in the scanner. Started from ctest, the process didn't inherit the error mode that keeps Windows' own boxes away, so the load put up a "Bad Image" box and waited for a click, and the test timed out. A CLAP library is now opened with those boxes off for that call (`SetThreadErrorMode`), wherever it's opened. The scanner already had them off for the whole process. 6b's in-process loading needs the same for every format.

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

## 6c: Tuning for plugins — `src/tuning/external/`

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
  - Can be saved as a `.syx` file or sent straight to a hardware port (6d).

## 6d: MIDI out, retuning hardware, External Instrument — `src/engine/midiout/`, `src/devices/external/`

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
  3. Optionally run the Tuning Verifier (6e), which **checks the bend range automatically**: it plays a known bend and measures the result.

  Profiles can be saved and reused, and they're shared through the library.

### External Instrument device
- One device that combines:
  - MIDI out (port, channel, and the retuning profile)
  - an **audio return** from the audio interface's inputs
  - a **hardware latency** setting in ms, which can also be **measured automatically** (see the Tuning Verifier)
- **Engine addition:** live audio input is passed through the track, with monitoring and delay compensation. This is also the base for audio tracks after the MVP.
- **External Audio Effect device:** sends audio to interface outputs and returns from inputs, with latency measurement, for outboard effects.

## 6e: Clock sync and the Tuning Verifier

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

## 6f: Crash isolation (sandboxing) — `src/hosting/sandbox/`
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
  - The helper's audio thread runs at real-time priority, as the engine's `WorkerPool` threads do: `juce::Thread::startRealtimeThread`, plus `platform::joinProAudioClass` (MMCSS on Windows). On Linux it uses rtkit or `SCHED_FIFO`; on macOS, the audio workgroup.
  - **Cross-process signaling** goes through a new `platform::IpcSignal`: named events on Windows, futex or POSIX semaphores on Linux, with shared memory from `shm_open`.
  - The plugin's window still opens from the helper process, as a normal window.
- **If a plugin crashes:**
  - Only that device goes silent and shows an error badge. The app and every other track keep playing.
  - You can reload the device with its last saved state.
  - The crash is logged with the plugin's name and version, and the plugin can be added to the blocklist.
- **Cost:** the extra CPU per isolated plugin, and per process, is benchmarked in `lattice_bench`. It's shown in the UI so you can choose a mode knowing the cost.

## 6g: The quality pass
- **Per-device CPU:** every device's time per block, built-in ones included, shown on its title bar and in the CPU meter's hover panel. Its cost goes into the bench baselines.
- **The reference set** gets six test plugins, as the charter asks. That includes the latency-changing plugin and the multi-out plugin, so delay compensation and the extra outputs are part of every measurement.
- **The soak** (`perf-check.ps1 -Soak`) gets plugins:
  - plugin windows open and closed during the UI stress;
  - a plugin's latency changed every few seconds;
  - MIDI out to a loopback port, with its jitter in the result lines.
- **The quiet-machine session** (arranged with the user first). It measures:
  - hosting overhead;
  - the sandbox handoff in each mode;
  - MIDI-out jitter at p99 over 10 minutes;
  - loading the reference set with its plugins.

  The results go into `perf/reference-machine.md`.
- **Callgrind** on Linux for the hosting path and the MPE retuner, so later changes can be compared by instruction count.

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
