#pragma once

#include "control/surface/Mode.h"

#include <vector>

namespace lattice::control
{

/** Mix mode (docs/06-m5-controllers.md, "Mix mode"): the encoders control the mixer of the
    tracks in view (from the Session box's first track, as many as there are encoders).

    - The upper display buttons choose what they control: Volume, Pan, Sends (pressed again: the
      next return's send), or Track (the selected track's volume, pan and every send).
    - The lower display buttons select the tracks; with Stop Clip, Mute or Solo held they stop,
      mute or solo them (as track buttons do in Session mode).
    - The display shows each track's name and colour, the value and a knob, and its meter.
    - A device without encoders gets its grid's columns as the tracks' faders; the scene buttons
      choose Volume, Pan and each send, and the left and right arrows move along the tracks. */
class MixMode final : public Mode
{
public:
    explicit MixMode(ModeHost& host);

    enum class Control
    {
        volume,
        pan,
        send,
        track
    };

    ModeKind kind() const override { return ModeKind::mix; }
    bool handle(const SurfaceInput& input) override;
    void render(LedFrame& frame) override;
    void describe(SurfaceText& text) override;
    void encoderTargets(std::vector<juce::String>& targets) override;
    void display(DisplayFrame& frame) override;
    std::uint32_t aspects() const override
    {
        return ui::EngineStateFeed::transportAspect | ui::EngineStateFeed::slotsAspect | ui::EngineStateFeed::metersAspect;
    }

    Control control() const noexcept { return what; }
    /** The return whose sends the encoders control (Control::send). */
    int send() const noexcept { return sendIndex; }
    void setControl(Control c, int sendReturn = 0);
    /** The tracks the encoders (or columns) control, in order. */
    std::vector<int> tracks() const;
    int controls() const;

    /** The choices on the upper buttons (or the scene buttons, without encoders), in order. */
    struct Choice
    {
        Control control;
        int send = 0;
        juce::String label;
    };
    std::vector<Choice> choices() const;

private:
    bool padsOwned() const { return caps().encoders <= 0; }
    int selectedTrack() const;
    void press(const Choice& c);
    bool isChosen(const Choice& c) const;
    juce::String controlName(Control c, int send) const;

    Control what = Control::volume;
    int sendIndex = 0;
};

} // namespace lattice::control
