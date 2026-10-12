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

void SaturatorDevice::prepare(double sr, int maxBlock)
{
    for (auto& c : channels)
    {
        c.stageA = std::make_unique<dsp::FirHalfBand>(stageATaps, attenuation);
        c.stageB = std::make_unique<dsp::FirHalfBand>(stageBTaps, attenuation);
        c.stageA->prepareBlocks(maxBlock);
        c.stageB->prepareBlocks(2 * maxBlock);
    }
    // 2x: 2n samples, plus one for the 4x path's half-sample shift.
    up2.assign(static_cast<std::size_t>(2 * maxBlock + 1), 0.0f);
    up4.assign(static_cast<std::size_t>(4 * maxBlock), 0.0f);
    wetScratch.assign(static_cast<std::size_t>(maxBlock), 0.0f);
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

void SaturatorDevice::shapeBlock(Curve c, float* x, int count, int factor, float drive0, float driveStep) noexcept
{
    // The curve is chosen once per block; the drive ramps once per base sample.
    const auto apply = [&](auto&& curve) {
        for (int i = 0, base = 0; i < count; i += factor, ++base)
        {
            const float drive = drive0 + driveStep * static_cast<float>(base + 1);
            for (int k = i; k < i + factor; ++k)
                x[k] = curve(x[k] * drive);
        }
    };
    switch (c)
    {
    case Curve::Soft:
        apply([](float v) { return dsp::fastTanh(v); });
        return;
    case Curve::Hard:
        apply([](float v) { return std::clamp(v, -1.0f, 1.0f); });
        return;
    case Curve::Tube:
    case Curve::SineFold:
        apply([c](float v) { return shape(c, v); });
        return;
    }
}

void SaturatorDevice::wetBlock(Channel& c, const float* in, int n, bool four, Curve curveType, float drive0,
                               float driveStep, float* wet) noexcept
{
    float* a = up2.data();
    c.stageA->upsampleBlock(in, n, a); // 2n at 2x
    if (!four)
    {
        shapeBlock(curveType, a, 2 * n, 2, drive0, driveStep);
        c.stageA->downsampleBlock(a, n, wet);
        // Padded to the 4x path's latency, so switching never shifts the sound.
        for (int i = 0; i < n; ++i)
        {
            const float y = c.pad[static_cast<std::size_t>(c.padPos)];
            c.pad[static_cast<std::size_t>(c.padPos)] = wet[i];
            if (++c.padPos == padLength)
                c.padPos = 0;
            wet[i] = y;
        }
        return;
    }
    float* b = up4.data();
    c.stageB->upsampleBlock(a, 2 * n, b); // 4n at 4x
    shapeBlock(curveType, b, 4 * n, 4, drive0, driveStep);
    // Back to 2x, one 2x sample later (that makes the latency whole): the first goes into a[1].
    c.stageB->downsampleBlock(b, 2 * n, a + 1);
    a[0] = c.half;
    c.half = a[2 * n];
    c.stageA->downsampleBlock(a, n, wet);
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
        float* wetBlockOut = wetScratch.data();
        wetBlock(c, x, n, four, curveType, d0, driveStep, wetBlockOut);
        float out = o0;
        float mix = m0;
        for (int i = 0; i < n; ++i)
        {
            out += outputStep;
            mix += mixStep;
            const float in = x[i];
            float wet = wetBlockOut[i];
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
            if (++c.dryPos == latency)
                c.dryPos = 0;
            x[i] = dry + mix * (wet * out - dry);
        }
    }
    driveValue = targetDrive;
    outputValue = targetOutput;
    mixValue = targetMix;
}

} // namespace lattice
