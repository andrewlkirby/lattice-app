#pragma once

#include "control/ComputerKeyboardInput.h"
#include "engine/AudioEngine.h"
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
    explicit TuningKeyboardComponent(juce::MidiKeyboardState& state);

    void setTuning(TuningPtr t);

private:
    juce::String getWhiteNoteText(int midiNoteNumber) override;

    TuningPtr tuning;
};

/** M1 controls for playing tunings (replaced by the real UI in M4): tuning menu, "Load tuning
    file...", the computer keyboard toggle with its range and velocity, the last-note readout, and
    an on-screen keyboard. All note sources feed the engine's MIDI input queue. */
class PlayPanel final : public juce::Component,
                        private juce::Timer,
                        private juce::MidiKeyboardState::Listener,
                        private TuningRegistry::Listener
{
public:
    PlayPanel(AudioEngine& engine, juce::PropertiesFile& settings, TuningRegistry& registry);
    ~PlayPanel() override;

    ComputerKeyboardInput& computerKeyboard() noexcept { return keyboardInput; }

    /** Selects a tuning for the track (and remembers it). */
    void selectTuning(const TuningPtr& tuning);

    void paint(juce::Graphics& g) override;
    void resized() override;

    static constexpr auto tuningIdKey = "tuningId";
    static constexpr auto userTuningFilesKey = "userTuningFiles";
    static constexpr auto computerKeyboardKey = "computerKeyboardOn";

private:
    void timerCallback() override;
    void handleNoteOn(juce::MidiKeyboardState*, int channel, int note, float velocity) override;
    void handleNoteOff(juce::MidiKeyboardState*, int channel, int note, float velocity) override;
    void tuningsChanged() override;

    void rebuildTuningMenu();
    void chooseTuningFile();
    void updateKeyboardInfo();

    AudioEngine& engine;
    juce::PropertiesFile& settings;
    TuningRegistry& registry;

    juce::MidiKeyboardState keyboardState;
    ComputerKeyboardInput keyboardInput;

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
