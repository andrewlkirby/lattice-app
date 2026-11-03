#pragma once

#include "control/surface/Surface.h"

#include <utility>
#include <vector>

namespace lattice::test
{

/** A device with any capabilities (docs/06-m5-controllers.md, Tests 1): it keeps the LED frames
    it's shown, and tests press its pads and buttons. */
class FakeSurface final : public control::SurfaceDriver
{
public:
    explicit FakeSurface(control::SurfaceCapabilities c) : caps(std::move(c)) {}

    const control::SurfaceCapabilities& capabilities() const override { return caps; }
    void showLeds(const control::LedFrame& frame, const control::LedFrame::Changes& changed) override
    {
        leds = frame;
        lastChanges = changed;
        ++frames;
    }

    void send(const control::SurfaceInput& input)
    {
        if (onInput)
            onInput(input);
    }
    void press(int row, int column, float velocity = 0.8f) { send(control::SurfaceInput::padDown(row, column, velocity)); }
    void release(int row, int column) { send(control::SurfaceInput::padUp(row, column)); }
    void tap(int row, int column)
    {
        press(row, column);
        release(row, column);
    }
    void down(control::Role role, int index = 0) { send(control::SurfaceInput::buttonDown({role, index})); }
    void up(control::Role role, int index = 0) { send(control::SurfaceInput::buttonUp({role, index})); }
    void click(control::Role role, int index = 0)
    {
        down(role, index);
        up(role, index);
    }

    const control::LedState& pad(int row, int column) const
    {
        return leds.pads[static_cast<std::size_t>(row * caps.columns + column)];
    }
    /** A button's LED (off if the device has no such button). */
    control::LedState button(control::Role role, int index = 0) const
    {
        const int i = caps.indexOf({role, index});
        return i < 0 ? control::LedState{} : leds.buttons[static_cast<std::size_t>(i)];
    }

    control::SurfaceCapabilities caps;
    control::LedFrame leds;
    control::LedFrame::Changes lastChanges;
    int frames = 0;
};

} // namespace lattice::test
