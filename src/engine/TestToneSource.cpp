#include "engine/TestToneSource.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lattice
{

void TestToneSource::prepare(double newSampleRate)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    sampleRate = newSampleRate;
    phase = 0.0;
    smoothedGain.reset(sampleRate, gainRampSeconds);
    smoothedGain.setCurrentAndTargetValue(isEnabled() ? getGain() : 0.0f);
}

void TestToneSource::render(float* out, int numSamples) noexcept
{
    constexpr double twoPi = 2.0 * std::numbers::pi;

    smoothedGain.setTargetValue(isEnabled() ? getGain() : 0.0f);
    const double hz = std::clamp(static_cast<double>(getFrequency()), 0.0, 0.49 * sampleRate);
    const double increment = twoPi * hz / sampleRate;

    if (!smoothedGain.isSmoothing() && smoothedGain.getCurrentValue() == 0.0f)
    {
        // Silent: keep the phase moving so the next start is as continuous as a running oscillator.
        std::fill(out, out + numSamples, 0.0f);
        phase = std::fmod(phase + increment * numSamples, twoPi);
        return;
    }

    for (int i = 0; i < numSamples; ++i)
    {
        out[i] = smoothedGain.getNextValue() * static_cast<float>(std::sin(phase));
        phase += increment;
        if (phase >= twoPi)
            phase -= twoPi;
    }
}

} // namespace lattice
