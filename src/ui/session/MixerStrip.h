#pragma once

#include "engine/SessionController.h"
#include "ui/feed/EngineStateFeed.h"
#include "ui/widgets/Fader.h"
#include "ui/widgets/Knob.h"
#include "ui/widgets/NumberBox.h"
#include "ui/widgets/Toggle.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace lattice::ui
{

/** One column of the Session view's mixer (docs/05-m4-ui.md, 4b, "Mixer section"), under its
    track, return or the master:

    - Sends: a knob per return, with the return's name and the level in dB (the M3 hands-on
      checks found unlabelled sends hard to use).
    - Main: pan; the fader with its meter and a dB box; the track activator (lit while the track
      is on, showing its number), solo, and arm (not on a group or a return); "Pre" on a return
      (its sends are taken before the faders). A track that feeds a sidechain shows "SC".

    Every control is a bound widget, so it's one undo step per gesture and follows the document.
    The meter follows the engine state feed. */
class MixerStrip final : public juce::Component
{
public:
    enum class Kind
    {
        track,
        returnTrack,
        master
    };

    MixerStrip(SessionController& session, Kind kind, int index);

    Kind kind() const noexcept { return stripKind; }
    int index() const noexcept { return stripIndex; }
    void setShowsSends(bool show);

    /** The meter's levels for this frame (the Session view hands them over from the feed). */
    void showLevels(std::array<float, 2> peaks, double frameSeconds);

    /** The height the strip wants with sends shown or not, in the theme's sizes. */
    static int heightFor(const juce::Component& themed, int numReturns, bool showSends);

    Fader* fader() noexcept { return volume.get(); }
    Knob* panKnob() noexcept { return pan.get(); }
    NumberBox* volumeBox() noexcept { return volumeText.get(); }
    Toggle* activator() noexcept { return on.get(); }
    Toggle* soloButton() noexcept { return solo.get(); }
    Toggle* armButton() noexcept { return arm.get(); }
    Toggle* preFaderButton() noexcept { return pre.get(); }
    const std::vector<std::unique_ptr<Knob>>& sendKnobs() const noexcept { return sends; }
    bool feedsSidechain() const noexcept { return sidechainBadge; }

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    SessionController& session;
    Kind stripKind;
    int stripIndex;
    bool showSends = true;
    bool sidechainBadge = false;

    std::vector<std::unique_ptr<Knob>> sends;
    std::unique_ptr<Knob> pan;
    std::unique_ptr<Fader> volume;
    std::unique_ptr<NumberBox> volumeText;
    std::unique_ptr<Toggle> on, solo, arm, pre;
};

} // namespace lattice::ui
