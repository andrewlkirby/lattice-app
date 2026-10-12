#pragma once

#include "control/ComputerKeyboardInput.h"
#include "engine/AudioEngine.h"
#include "engine/SessionController.h"
#include "tuning/TuningRegistry.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>

namespace lattice
{

/** On-screen keyboard that labels keys with table indices when the tuning has no note names. */
class TuningKeyboardComponent final : public juce::MidiKeyboardComponent
{
public:
    explicit TuningKeyboardComponent(juce::MidiKeyboardState& keyboardState);

    void setTuning(TuningPtr t);

private:
    juce::String getWhiteNoteText(int midiNoteNumber) override;

    TuningPtr tuning;
};

/** Playing controls for the selected track (replaced by the real UI in M4): its tuning menu, "Load
    tuning file...", the computer keyboard toggle with its range and velocity, the last-note
    readout, and an on-screen keyboard. All note sources feed the engine's live MIDI input, which
    plays the armed tracks. */
class PlayPanel final : public juce::Component,
                        private juce::Timer,
                        private juce::MidiKeyboardState::Listener,
                        private TuningRegistry::Listener,
                        private juce::ValueTree::Listener
{
public:
    PlayPanel(SessionController& session, AudioEngine& engine, juce::PropertiesFile& settings, TuningRegistry& registry);
    ~PlayPanel() override;

    ComputerKeyboardInput& computerKeyboard() noexcept { return keyboardInput; }

    /** The track whose tuning the panel shows and edits. */
    void setTrack(int trackIndex);

    /** Sets the selected track's tuning (one undo step) and remembers it for new sets. */
    void selectTuning(const TuningPtr& tuning);

    void paint(juce::Graphics& g) override;
    void resized() override;

    static constexpr auto tuningIdKey = "tuningId";
    static constexpr auto userTuningFilesKey = "userTuningFiles";
    static constexpr auto computerKeyboardKey = "computerKeyboardOn";

    /** Loads the tuning files the user loaded before (call before the session is created). */
    static void loadRememberedTunings(TuningRegistry& registry, const juce::PropertiesFile& settings);

private:
    void timerCallback() override;
    void handleNoteOn(juce::MidiKeyboardState*, int channel, int note, float velocity) override;
    void handleNoteOff(juce::MidiKeyboardState*, int channel, int note, float velocity) override;
    void tuningsChanged() override;
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override { needsRefresh = true; }
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { needsRefresh = true; }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { needsRefresh = true; }
    void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override { needsRefresh = true; }

    TuningPtr trackTuning() const;
    void refresh();
    void rebuildTuningMenu();
    void chooseTuningFile();
    void updateKeyboardInfo();

    SessionController& session;
    AudioEngine& engine;
    juce::PropertiesFile& settings;
    TuningRegistry& registry;
    int track = 0;
    TuningPtr configuredTuning;
    bool needsRefresh = true;

    juce::MidiKeyboardState keyboardState;
    ComputerKeyboardInput keyboardInput;

    juce::Label trackLabel;
    juce::Label tuningLabel{{}, "Tuning"};
    juce::ComboBox tuningBox;
    juce::TextButton loadButton{"Load tuning file..."};
    juce::ToggleButton keyboardToggle{"Computer keyboard (M)"};
    juce::Label keyboardInfo;
    juce::Label lastNoteLabel;
    TuningKeyboardComponent keyboard{keyboardState};
    std::unique_ptr<juce::FileChooser> chooser;
    int shownLastNote = -2;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(PlayPanel)
};

} // namespace lattice
