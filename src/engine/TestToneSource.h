#pragma once

#include "core/RealtimeCheck.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>

namespace lattice
{

/** Sine test tone. Settings may be changed from any thread; render() runs on the audio thread.
    Gain (including on/off) is smoothed so that changes never click. */
class TestToneSource
{
public:
    static constexpr double gainRampSeconds = 0.02;

    void setEnabled(bool shouldPlay) noexcept { enabled.store(shouldPlay, std::memory_order_relaxed); }
    bool isEnabled() const noexcept { return enabled.load(std::memory_order_relaxed); }

    void setFrequency(float hz) noexcept { frequency.store(hz, std::memory_order_relaxed); }
    float getFrequency() const noexcept { return frequency.load(std::memory_order_relaxed); }

    /** Linear gain, 0..1. */
    void setGain(float linearGain) noexcept { gain.store(linearGain, std::memory_order_relaxed); }
    float getGain() const noexcept { return gain.load(std::memory_order_relaxed); }

    /** Not on the audio thread. Resets phase and snaps the gain to its current target. */
    void prepare(double sampleRate);

    /** Overwrites numSamples of out with the tone. */
    void render(float* out, int numSamples) noexcept LATTICE_NONBLOCKING;

private:
    std::atomic<bool> enabled{false};
    std::atomic<float> frequency{440.0f};
    std::atomic<float> gain{0.25f};

    double sampleRate = 48000.0;
    double phase = 0.0; // radians, kept in [0, 2pi)
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedGain;
};

} // namespace lattice
