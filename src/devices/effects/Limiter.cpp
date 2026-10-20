#include "devices/effects/Limiter.h"

#include <algorithm>
#include <cmath>

namespace lattice
{
namespace
{
using P = LimiterDevice::Param;

std::vector<ParameterSpec> makeSpecs()
{
    std::vector<ParameterSpec> s(static_cast<std::size_t>(P::numParams));
    const auto set = [&](int index, ParameterSpec spec) {
        spec.group = "Limiter";
        s[static_cast<std::size_t>(index)] = std::move(spec);
    };
    set(P::gain, ParameterSpec::continuous("limiter.gain", "Gain", 0.0f, 24.0f, 0.0f, ParamUnit::Decibels));
    set(P::ceiling, ParameterSpec::continuous("limiter.ceiling", "Ceiling", -20.0f, 0.0f, -0.3f, ParamUnit::Decibels));
    set(P::release,
        ParameterSpec::continuous("limiter.release", "Release", 1.0f, 1000.0f, 50.0f, ParamUnit::Milliseconds).withCentre(50.0f));
    return s;
}
} // namespace

LimiterDevice::LimiterDevice() : StudioDevice(typeId, "Limiter", DeviceKind::AudioEffect, makeSpecs())
{
}

void LimiterDevice::prepare(double sr, int)
{
    window = std::max(2, static_cast<int>(std::lround(lookaheadSeconds * sr)) + 1);
    setLatency(window - 1);
    delayL.assign(static_cast<std::size_t>(window), 0.0f);
    delayR.assign(static_cast<std::size_t>(window), 0.0f);
    minValues.assign(static_cast<std::size_t>(window + 1), 1.0f);
    minTimes.assign(static_cast<std::size_t>(window + 1), 0);
    averageRing.assign(static_cast<std::size_t>(window), 1.0f);
    reset();
}

void LimiterDevice::reset() noexcept
{
    std::fill(delayL.begin(), delayL.end(), 0.0f);
    std::fill(delayR.begin(), delayR.end(), 0.0f);
    std::fill(averageRing.begin(), averageRing.end(), 1.0f);
    averageSum = static_cast<double>(window);
    delayPos = averagePos = 0;
    minHead = minCount = 0;
    time = 0;
    envelope = 1.0f;
}

void LimiterDevice::process(DeviceBuffers& io, const DeviceTimeInfo&) noexcept
{
    const int n = io.numSamples;
    float* l = io.channels[0];
    float* r = io.channels[1];
    const float targetGain = std::pow(10.0f, value(P::gain) / 20.0f);
    const float ceilingGain = std::pow(10.0f, value(P::ceiling) / 20.0f) * 0.99999f; // a hair of margin for rounding
    const auto releaseCoef = static_cast<float>(1.0 - std::exp(-1.0 / (std::max(1.0f, value(P::release)) * 0.001 * sampleRate())));
    const float gainStep = (targetGain - inputGain) / static_cast<float>(n);
    const int capacity = window + 1;
    float lowest = 1.0f;

    for (int i = 0; i < n; ++i)
    {
        inputGain += gainStep;
        const float xl = l[i] * inputGain;
        const float xr = r[i] * inputGain;
        const float peak = std::max(std::abs(xl), std::abs(xr));
        const float need = peak > ceilingGain ? ceilingGain / peak : 1.0f;

        // Sliding minimum of `need` over the last `window` samples.
        while (minCount > 0)
        {
            const int back = (minHead + minCount - 1) % capacity;
            if (minValues[static_cast<std::size_t>(back)] < need)
                break;
            --minCount;
        }
        const int slot = (minHead + minCount) % capacity;
        minValues[static_cast<std::size_t>(slot)] = need;
        minTimes[static_cast<std::size_t>(slot)] = time;
        ++minCount;
        if (minTimes[static_cast<std::size_t>(minHead)] <= time - window)
        {
            minHead = (minHead + 1) % capacity;
            --minCount;
        }
        const float windowMin = minValues[static_cast<std::size_t>(minHead)];

        // Recover towards unity at the release rate, never above what the window needs.
        envelope = std::min(windowMin, envelope + (1.0f - envelope) * releaseCoef);

        // Average over the window: the gain glides down to each peak over the look-ahead.
        averageSum += static_cast<double>(envelope) - static_cast<double>(averageRing[static_cast<std::size_t>(averagePos)]);
        averageRing[static_cast<std::size_t>(averagePos)] = envelope;
        averagePos = (averagePos + 1) % window;
        const auto g = static_cast<float>(averageSum / window);
        lowest = std::min(lowest, g);

        // The audio, delayed by window - 1 samples, so the peak arrives as the gain reaches it.
        const int readPos = (delayPos + 1) % window;
        delayL[static_cast<std::size_t>(delayPos)] = xl;
        delayR[static_cast<std::size_t>(delayPos)] = xr;
        l[i] = delayL[static_cast<std::size_t>(readPos)] * g;
        r[i] = delayR[static_cast<std::size_t>(readPos)] * g;
        delayPos = readPos;
        ++time;
    }
    reductionDb.store(20.0f * std::log10(std::max(lowest, 1e-6f)), std::memory_order_relaxed);
}

} // namespace lattice
