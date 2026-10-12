#include "devices/effects/Saturator.h"

#include "devices/dsp/Ladder.h"

#include <algorithm>
#include <cmath>

namespace lattice
{
namespace
{
using P = SaturatorDevice::Param;
constexpr int stageATaps = 59;  // flat to 0.2 of the 2x rate, -90 dB from 0.3
constexpr int stageBTaps = 23;  // at 4x the images are far away: a short filter does
constexpr double attenuation = 90.0;

std::vector<ParameterSpec> makeSpecs()
{
    std::vector<ParameterSpec> s(static_cast<std::size_t>(P::numParams));
    const auto set = [&](int index, ParameterSpec spec) {
        spec.group = "Saturator";
        s[static_cast<std::size_t>(index)] = std::move(spec);
    };
    set(P::curve, ParameterSpec::choice("sat.curve", "Curve", {"Soft", "Hard", "Tube", "Sine Fold"}, 0));
    set(P::drive, ParameterSpec::continuous("sat.drive", "Drive", 0.0f, 36.0f, 6.0f, ParamUnit::Decibels));
    set(P::output, ParameterSpec::continuous("sat.output", "Output", -36.0f, 6.0f, -3.0f, ParamUnit::Decibels));
    set(P::oversampling, ParameterSpec::choice("sat.oversampling", "Oversampling", {"2x", "4x"}, 0));
    set(P::mix, ParameterSpec::continuous("sat.mix", "Dry/Wet", 0.0f, 1.0f, 1.0f, ParamUnit::Percent).withFormatter([](float v) {
        return juce::String(juce::roundToInt(v * 100.0f)) + " %";
    }));
    return s;
}
} // namespace

SaturatorDevice::SaturatorDevice() : StudioDevice(typeId, "Saturator", DeviceKind::AudioEffect, makeSpecs())
{
}

float SaturatorDevice::shape(Curve c, float x) noexcept
{
    switch (c)
    {
    case Curve::Soft:
        return dsp::fastTanh(x);
    case Curve::Hard:
        return std::clamp(x, -1.0f, 1.0f);
    case Curve::Tube:
    {
        // Biased into an asymmetric region of tanh: even harmonics, like a single-ended stage.
        constexpr float bias = 0.3f;
        static const float offset = std::tanh(bias);
        return std::tanh(x + bias) - offset;
    }
    case Curve::SineFold:
        return std::sin(x * 1.5707963f);
    }
    return x;
}

void SaturatorDevice::prepare(double sr, int)
{
    for (auto& c : channels)
    {
        c.stageA = std::make_unique<dsp::FirHalfBand>(stageATaps, attenuation);
        c.stageB = std::make_unique<dsp::FirHalfBand>(stageBTaps, attenuation);
    }
    // 4x: stage A's round trip, stage B's (in 2x samples, so half in base samples), plus the extra
    // 2x sample that makes it whole.
    const int a = channels[0].stageA->roundTripLatency();
    const int b2x = channels[0].stageB->roundTripLatency(); // in samples at 2x
    latency = a + (b2x + 1) / 2;
    padLength = latency - a;
    for (auto& c : channels)
    {
        c.pad.assign(static_cast<std::size_t>(std::max(1, padLength)), 0.0f);
        c.dry.assign(static_cast<std::size_t>(std::max(1, latency)), 0.0f);
    }
    setLatency(latency);
    dcCoefficient = static_cast<float>(std::exp(-2.0 * 3.14159265358979 * 10.0 / sr));
    reset();
}

void SaturatorDevice::reset() noexcept
{
    for (auto& c : channels)
    {
        if (c.stageA != nullptr)
        {
            c.stageA->reset();
            c.stageB->reset();
        }
        c.half = 0.0f;
        std::fill(c.pad.begin(), c.pad.end(), 0.0f);
        std::fill(c.dry.begin(), c.dry.end(), 0.0f);
        c.padPos = c.dryPos = 0;
        c.dcIn = c.dcOut = 0.0f;
    }
}

float SaturatorDevice::wet2x(Channel& c, float x, Curve curveType, float drive) noexcept
{
    float a0 = 0.0f;
    float a1 = 0.0f;
    c.stageA->upsample(x, a0, a1);
    const float y = c.stageA->downsample(shape(curveType, a0 * drive), shape(curveType, a1 * drive));
    // Padded to the 4x path's latency.
    const float out = c.pad[static_cast<std::size_t>(c.padPos)];
    c.pad[static_cast<std::size_t>(c.padPos)] = y;
    c.padPos = (c.padPos + 1) % padLength;
    return out;
}

float SaturatorDevice::wet4x(Channel& c, float x, Curve curveType, float drive) noexcept
{
    float a[2];
    c.stageA->upsample(x, a[0], a[1]);
    float down[2];
    for (int k = 0; k < 2; ++k)
    {
        float b0 = 0.0f;
        float b1 = 0.0f;
        c.stageB->upsample(a[k], b0, b1);
        down[k] = c.stageB->downsample(shape(curveType, b0 * drive), shape(curveType, b1 * drive));
    }
    // One sample of delay at 2x (half a base sample) makes the latency whole.
    const float y0 = c.half;
    c.half = down[1];
    return c.stageA->downsample(y0, down[0]);
}

void SaturatorDevice::process(DeviceBuffers& io, const DeviceTimeInfo&) noexcept
{
    const int n = io.numSamples;
    const auto curveType = static_cast<Curve>(juce::roundToInt(value(P::curve)));
    const bool four = juce::roundToInt(value(P::oversampling)) == 1;
    const float targetDrive = std::pow(10.0f, value(P::drive) / 20.0f);
    const float targetOutput = std::pow(10.0f, value(P::output) / 20.0f);
    const float targetMix = value(P::mix);
    const float driveStep = (targetDrive - driveValue) / static_cast<float>(n);
    const float outputStep = (targetOutput - outputValue) / static_cast<float>(n);
    const float mixStep = (targetMix - mixValue) / static_cast<float>(n);
    const float d0 = driveValue, o0 = outputValue, m0 = mixValue;

    for (int ch = 0; ch < 2; ++ch)
    {
        auto& c = channels[ch];
        float* x = io.channels[ch];
        float drive = d0;
        float out = o0;
        float mix = m0;
        for (int i = 0; i < n; ++i)
        {
            drive += driveStep;
            out += outputStep;
            mix += mixStep;
            const float in = x[i];
            float wet = four ? wet4x(c, in, curveType, drive) : wet2x(c, in, curveType, drive);
            if (curveType == Curve::Tube)
            {
                const float y = wet - c.dcIn + dcCoefficient * c.dcOut;
                c.dcIn = wet;
                c.dcOut = y;
                wet = y;
            }
            // The dry signal, delayed to line up with the wet one.
            const float dry = c.dry[static_cast<std::size_t>(c.dryPos)];
            c.dry[static_cast<std::size_t>(c.dryPos)] = in;
            c.dryPos = (c.dryPos + 1) % latency;
            x[i] = dry + mix * (wet * out - dry);
        }
    }
    driveValue = targetDrive;
    outputValue = targetOutput;
    mixValue = targetMix;
}

} // namespace lattice
