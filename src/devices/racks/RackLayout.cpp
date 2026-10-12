#include "devices/racks/RackLayout.h"

#include <algorithm>
#include <cmath>

namespace lattice
{

void RackChainState::setMix(double volumeDb, double pan, bool mute, bool solo) noexcept
{
    const float gain = volumeDb <= -70.0 ? 0.0f : static_cast<float>(std::pow(10.0, volumeDb / 20.0));
    const double angle = (std::clamp(pan, -1.0, 1.0) + 1.0) * 0.25 * 3.14159265358979323846;
    volumeLeft.store(gain * static_cast<float>(std::cos(angle)), std::memory_order_relaxed);
    volumeRight.store(gain * static_cast<float>(std::sin(angle)), std::memory_order_relaxed);
    muted.store(mute, std::memory_order_relaxed);
    soloed.store(solo, std::memory_order_relaxed);
}

void RackChainState::prepare(int maxBlockSize)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    blockSize = std::max(1, maxBlockSize);
    audio.setSize(2, blockSize, false, true, false);
    audio.clear();
    midiStorage.assign(static_cast<std::size_t>(maxEvents), TimedMidi{});
    runner.prepare(maxEvents);
}

CompensationDelay::CompensationDelay(int delaySamples) : samples(std::max(0, delaySamples))
{
    for (auto& line : lines)
        line.prepare(samples + 4);
}

void CompensationDelay::process(float* const* channels, int numSamples) noexcept
{
    for (int ch = 0; ch < 2; ++ch)
    {
        float* x = channels[ch];
        auto& line = lines[ch];
        for (int i = 0; i < numSamples; ++i)
        {
            line.push(x[i]);
            x[i] = line.tap(samples);
        }
    }
}

float RackChainLayout::zoneGain(int note, int velocity) const noexcept
{
    const auto ramp = [](int x, int low, int high, int fadeLow, int fadeHigh) {
        if (x < low || x > high)
            return 0.0f;
        float g = 1.0f;
        if (fadeLow > 0 && x < low + fadeLow)
            g = std::min(g, static_cast<float>(x - low + 1) / static_cast<float>(fadeLow + 1));
        if (fadeHigh > 0 && x > high - fadeHigh)
            g = std::min(g, static_cast<float>(high - x + 1) / static_cast<float>(fadeHigh + 1));
        return g;
    };
    return ramp(note, keyLow, keyHigh, keyFadeLow, keyFadeHigh) *
           ramp(velocity, velocityLow, velocityHigh, velocityFadeLow, velocityFadeHigh);
}

float MacroMapping::valueFor(float macroValue) const noexcept
{
    float x = std::clamp(macroValue / 127.0f, 0.0f, 1.0f);
    // Curve: x^(2^(2c)), so c = 1 starts slowly (x^4) and c = -1 quickly (x^0.25).
    if (curve != 0.0f)
        x = std::pow(x, std::exp2(2.0f * curve));
    const float normalised = minimum + (maximum - minimum) * x;
    return target->getNormalisableRange().convertFrom0to1(std::clamp(normalised, 0.0f, 1.0f));
}

void RackLayout::finalise()
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    latencySamples = 0;
    for (auto& c : chains)
    {
        c.chain.finalise();
        latencySamples = std::max(latencySamples, c.chain.latencySamples);
    }
    for (auto& c : chains)
    {
        const int behind = latencySamples - c.chain.latencySamples;
        c.compensation = behind > 0 ? std::make_shared<CompensationDelay>(behind) : nullptr;
    }
}

} // namespace lattice
