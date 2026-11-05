#pragma once

#include "control/generic/MidiSurfaceDriver.h"
#include "control/push/PushDisplay.h"
#include "control/push/PushPalette.h"
#include "core/LatencyStats.h"
#include "ui/theme/Theme.h"

#include <juce_events/juce_events.h>

#include <array>
#include <functional>
#include <memory>

namespace lattice::control::push
{

/** Ableton Push 2 (and Push 3 in controller mode, unverified) on its Live port
    (docs/06-m5-controllers.md, "Ableton Push 2"):

    - Input: pads (notes 36-99, with polyphonic pressure), the buttons Lattice has roles for, the
      eight encoders (two's complement, with touch), the tempo encoder and the touch strip. What
      it doesn't use (Select, Page, User, the master and swing encoders) passes on to MIDI Map.
    - Lights: each colour loaded into the device's palette (Palette) and the LED set to it; a
      pulse or a blink is the device's own animation, kept on the beat by MIDI clock. Changes go
      as a batch a frame, no more than `bytesPerFrame` of MIDI a frame, so the device's input is
      never flooded; what's left goes the next frame.
    - The display: each frame drawn by DisplayRenderer at 960 x 160 and sent over USB on its own
      thread (DisplaySender). Without the display (no driver, no rule) everything else works.

    Message thread, but for receive(). */
class PushDriver final : public generic::MidiSurfaceDriver, private juce::Timer
{
public:
    enum class Model : std::uint8_t
    {
        push2,
        push3
    };
    /** `opener` opens the display (tests: a fake; null: none). `beat` gives the beat the lights'
        animations follow (VirtualPush::beatNow's: the transport's, or the tempo's while
        stopped); null: a steady 120 BPM. */
    PushDriver(Model model, generic::MidiSender& output, LinkOpener opener, std::function<double()> beat = {});
    ~PushDriver() override;

    static constexpr int bytesPerFrame = 2048;

    const SurfaceCapabilities& capabilities() const override { return caps; }
    void showLeds(const LedFrame& frame, const LedFrame::Changes& changed) override;
    void showDisplay(const DisplayFrame& frame) override;

    /** Live mode, polyphonic pressure, the touch strip as pitch bend, full brightness, then every
        light as shown and the display. */
    void connect() override;
    /** Every light off (the display goes dark by itself). */
    void disconnect() override;

    bool receive(const juce::MidiMessage& message, double seconds = -1.0) override;
    juce::String describe(const juce::MidiMessage& message) const override;
    /** The display's trouble, if it has any. */
    juce::String status() const override;

    /** The display's colours (its accent and meters). */
    void setTheme(const ui::Theme& theme);

    /** Tests: sends what's waiting now (as the next frame's timer would), and moves the clock on. */
    void flush();
    void tick() { timerCallback(); }
    const Palette& palette() const noexcept { return colours; }
    DisplaySender* display() noexcept { return sender.get(); }
    /** The image the display was last given (tests look at it). */
    const juce::Image& displayImage() const noexcept { return image; }
    /** The MIDI clocks sent since the last start. */
    std::int64_t clocksSent() const noexcept { return clocks; }
    /** From lights changing to the last of them sent, in ms (docs/06-m5-controllers.md, 5g): at
        once, or over the next frames when a redraw is more than a frame's budget. */
    const LatencyStats& ledDelays() const noexcept { return ledsAfter; }
    LatencyStats& ledDelays() noexcept { return ledsAfter; }

private:
    /** One LED as the device has it: its entry, and the second entry and channel of an
        animation (an off LED: 0, 0, 0). */
    struct Shown
    {
        int base = 0, animated = 0, channel = 0;
        bool operator==(const Shown&) const = default;
    };
    struct Light
    {
        bool note = false; // a pad (a note), or a button (a CC)
        int number = 0;
        bool rgb = true;
        LedState wanted{};
        Shown device{};
        bool dirty = true;
    };
    void timerCallback() override;
    void startTimerIfNeeded();
    /** Sends one light; false if the frame's budget is spent. */
    bool sendLight(Light& light, int& budget);
    void sendClock();
    bool anyAnimatedShown() const noexcept;

    Model model;
    SurfaceCapabilities caps;
    generic::MidiSender& out;
    std::function<double()> beatNow;
    ui::Theme theme;

    // Lights: the pads (row by row from the top), then the capabilities' buttons.
    std::vector<Light> lights;
    Palette colours;
    std::size_t nextDirty = 0; // where the last frame's budget ran out
    double dirtySince = -1.0;  // ms: the first change not sent yet (-1: none)
    LatencyStats ledsAfter;

    // The animations' clock.
    bool clockStarted = false;
    std::int64_t clocks = 0; // since the start (0 at a whole beat)

    // The display.
    std::unique_ptr<DisplaySender> sender;
    DisplayFrame lastFrame;
    bool hasFrame = false;
    juce::Image image;

    // The input thread's.
    std::array<bool, 64> held{};
};

} // namespace lattice::control::push
