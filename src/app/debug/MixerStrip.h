#pragma once

#include "app/debug/SessionUi.h"
#include "engine/SessionController.h"
#include "tuning/TuningRegistry.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>
#include <vector>

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

/** One mixer strip (docs/03-m2-engine-session.md, "Mixer"; docs/04-m3-devices.md, "3f", "Mixer"):
    - a track: tuning, a send knob per return, volume, pan, mute, solo, arm and a meter;
    - a return: name, pre/post-fader sends, volume, pan, mute, solo and a meter;
    - the master: volume, pan, a meter and "+ Return".
    Clicking the strip's background shows its device chain. Ctrl+click on solo or arm adds to the
    current set instead of replacing it. */
class MixerStrip final : public juce::Component
{
public:
    enum class Kind : std::uint8_t
    {
        Track,
        Return,
        Master
    };

    MixerStrip(SessionController& session, TuningRegistry& tunings, Kind kind, int index);

    /** The strip's height for a set with this many returns (track strips hold a send for each). */
    static int heightFor(int numReturns);

    /** Reads the document (call when it changed). */
    void refresh();
    void rebuildTuningMenu();
    /** The UI tick: takes the engine's peaks into the meter. */
    void updateMeter();

    /** Clicked: show this strip's device chain. */
    std::function<void()> onSelect;
    void setHighlighted(bool on);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    juce::ValueTree node() const;

    SessionController& session;
    TuningRegistry& registry;
    const Kind kind;
    const int index;
    bool highlighted = false;

    juce::ComboBox tuning;
    std::vector<std::unique_ptr<juce::Slider>> sends;
    juce::Slider volume{juce::Slider::LinearVertical, juce::Slider::TextBoxBelow};
    juce::Slider pan{juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox};
    juce::TextButton mute{"M"};
    juce::TextButton solo{"S"};
    juce::TextButton arm{juce::String::fromUTF8("\xe2\x97\x8f")}; // ●
    juce::TextButton preFader{"Pre"};
    juce::TextButton addReturn{"+ Return"};
    Meter meter;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MixerStrip)
};

} // namespace lattice::debugui
