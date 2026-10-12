#pragma once

#include "app/debug/SessionUi.h"
#include "engine/SessionController.h"
#include "tuning/TuningRegistry.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

namespace lattice::debugui
{

/** A stereo peak meter, -60..+6 dB, with a falloff applied by the UI (the engine only reports the
    peak since the last read). Repaints only when what it shows changes. */
class Meter final : public juce::Component
{
public:
    void setPeaks(std::array<float, 2> peaks);
    void paint(juce::Graphics& g) override;

private:
    std::array<float, 2> shown{};
};

/** One track's mixer controls (docs/03-m2-engine-session.md, "Mixer"): tuning, volume, pan, mute,
    solo, arm and a meter. A trackIndex of -1 makes it the master strip (volume, pan, meter).
    Ctrl+click on solo or arm adds to the current set instead of replacing it. */
class MixerStrip final : public juce::Component
{
public:
    MixerStrip(SessionController& session, TuningRegistry& tunings, int trackIndex);

    /** Reads the document (call when it changed). */
    void refresh();
    void rebuildTuningMenu();
    /** The UI tick: takes the engine's peaks into the meter. */
    void updateMeter();

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    juce::ValueTree node() const;
    bool isMaster() const noexcept { return track < 0; }

    SessionController& session;
    TuningRegistry& registry;
    const int track;

    juce::ComboBox tuning;
    juce::Slider volume{juce::Slider::LinearVertical, juce::Slider::TextBoxBelow};
    juce::Slider pan{juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox};
    juce::TextButton mute{"M"};
    juce::TextButton solo{"S"};
    juce::TextButton arm{juce::String::fromUTF8("\xe2\x97\x8f")}; // ●
    Meter meter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerStrip)
};

} // namespace lattice::debugui
