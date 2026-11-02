#pragma once

#include "control/ComputerKeyboardInput.h"
#include "ui/commands/CommandRegistry.h"

#include <juce_data_structures/juce_data_structures.h>

namespace lattice
{
class AudioEngine;
class TuningRegistry;
} // namespace lattice

namespace lattice::ui::prefs
{

/** The settings the Preferences window edits (docs/05-m4-ui.md, 4g), their keys in the settings
    file, and how each one is applied, so startup and the window do it the same way. */

constexpr auto zoomKey = "ui.zoom";                         // 0.5 .. 2.5
constexpr auto multiThreadedKey = "multiThreadedAudio";     // bool
constexpr auto workersKey = "audioWorkers";                 // 0: one per core (the default)
constexpr auto defaultTuningKey = "tuningId";               // new tracks' tuning (empty: 12-TET)
constexpr auto defaultHighNotesKey = "newTrack.highNotes";  // new tracks' high-note policy
constexpr auto shortcutsKey = "shortcuts";                  // CommandRegistry::shortcutsToXml
constexpr auto remoteInputsKey = "midi.remoteInputs";       // inputs MIDI Map listens to, one per line
constexpr auto nonTrackInputsKey = "midi.notTrack";         // inputs open only for Remote (Track off)
constexpr auto noteKeysKey = "keyboard.noteKeys";           // the computer keyboard's 17 note keys
constexpr auto pageKeyPrefix = "keyboard.page.";            // + a tuning ID: its range step
constexpr auto userThemeKey = "theme.userFile";             // a JSON theme chosen from the Themes folder
constexpr auto deviceStateKey = "audioDeviceState";         // the audio device setup (AudioDeviceManager XML)
constexpr auto toneFrequencyKey = "testToneFrequency";      // Hz
constexpr auto toneLevelKey = "testToneLevelDb";            // dB
constexpr auto computerKeyboardKey = "computerKeyboardOn";  // bool: the computer keyboard plays notes
constexpr auto userTuningFilesKey = "userTuningFiles";      // tuning files loaded from elsewhere, one per line

constexpr double minZoom = 0.5, maxZoom = 2.5;

/** The zoom saved (1 if none), and applying it to every window (it multiplies the system's DPI
    scaling, so it follows each monitor). */
double zoom(const juce::PropertiesFile& settings);
void applyZoom(double zoom);

/** How many audio worker threads the settings ask for: none when multi-threading is off,
    otherwise the count chosen (0: one per core). */
int workerThreads(const juce::PropertiesFile& settings);

/** New tracks' tuning and high-note policy (LiveSet::newTrackDefaults), from the settings. */
void applyNewTrackDefaults(const juce::PropertiesFile& settings);

/** The shortcuts saved (only those changed from the defaults), and saving them on every change. */
void restoreShortcuts(const juce::PropertiesFile& settings, CommandRegistry& registry);
void saveShortcuts(juce::PropertiesFile& settings, const CommandRegistry& registry);

/** The computer keyboard's note keys and per-tuning range steps, from the settings. */
void applyKeyboard(const juce::PropertiesFile& settings, ComputerKeyboardInput& keyboard);
int pageSizeFor(const juce::PropertiesFile& settings, const juce::String& tuningId); // 0: the tuning's own

/** The test tone's frequency and level, from the settings (it stays off until switched on). */
void applyTestTone(const juce::PropertiesFile& settings, AudioEngine& engine);
/** Saves the audio device setup, unless the device is still opening. */
void saveDeviceState(juce::PropertiesFile& settings, AudioEngine& engine);

/** The tuning files loaded from outside the tuning folders before (call before the session is
    made, so a set's tracks find them), and remembering one more. */
void loadRememberedTunings(TuningRegistry& registry, const juce::PropertiesFile& settings);
void rememberTuningFile(juce::PropertiesFile& settings, const juce::File& file);

/** The MIDI inputs (by identifier) MIDI Map and Key Map listen to. */
juce::StringArray remoteInputs(const juce::PropertiesFile& settings);
void setRemoteInput(juce::PropertiesFile& settings, const juce::String& identifier, bool on);
/** Inputs open for Remote only: their notes don't reach the tracks. */
juce::StringArray nonTrackInputs(const juce::PropertiesFile& settings);
void setNonTrackInput(juce::PropertiesFile& settings, const juce::String& identifier, bool on);

} // namespace lattice::ui::prefs
