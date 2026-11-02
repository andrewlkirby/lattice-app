#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

namespace lattice::ui
{

/** A level meter (docs/05-m4-ui.md, "Widget library"): one bar per channel showing the RMS level
    solid and the peak level lighter behind it, a peak-hold line, and a clip light that stays lit
    until clicked. The scale is in dB from floorDb to topDb.

    Levels come in from the engine state feed once per display frame (setLevels), and advance()
    moves the peak hold on by the frame's time, so the meter's motion doesn't depend on the frame
    rate (and tests can drive it without a clock). */
class Meter final : public juce::Component
{
public:
    static constexpr int maxChannels = 2;
    static constexpr float floorDb = -60.0f;
    static constexpr float topDb = 6.0f;
    static constexpr double holdSeconds = 1.0;
    static constexpr float fallDbPerSecond = 20.0f;
    // Where the colours change.
    static constexpr float midDb = -18.0f;
    static constexpr float highDb = -6.0f;

    explicit Meter(int channels = 2);

    int channels() const noexcept { return numChannels; }
    /** Linear levels (1 = 0 dBFS). A peak over 1 lights the clip light. */
    void setLevels(int channel, float peak, float rms);
    void advance(double seconds);

    float peakDb(int channel) const;
    float rmsDb(int channel) const;
    float heldPeakDb(int channel) const;
    bool clipped() const noexcept { return clip; }
    void resetClip();

    /** 0 at floorDb (and below), 1 at topDb. */
    static float proportionForDb(float db) noexcept;
    static float toDb(float linear) noexcept;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override { resetClip(); }

private:
    struct Channel
    {
        float peak = 0.0f, rms = 0.0f; // linear
        float heldDb = floorDb;
        double heldFor = 0.0;
    };
    int numChannels;
    std::array<Channel, maxChannels> state{};
    bool clip = false;
};

} // namespace lattice::ui
