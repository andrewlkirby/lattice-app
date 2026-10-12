#include "devices/effects/Compressor.h"

#include <algorithm>
#include <cmath>

namespace lattice
{
namespace
{
using P = CompressorDevice::Param;

std::vector<ParameterSpec> makeSpecs()
{
    std::vector<ParameterSpec> s(static_cast<std::size_t>(P::numParams));
    const auto set = [&](int index, ParameterSpec spec, const char* group) {
        spec.group = group;
        s[static_cast<std::size_t>(index)] = std::move(spec);
    };
    const auto pct = [](float v) { return juce::String(juce::roundToInt(v * 100.0f)) + " %"; };
    set(P::threshold, ParameterSpec::continuous("comp.threshold", "Threshold", -60.0f, 0.0f, -18.0f, ParamUnit::Decibels),
        "Compressor");
    set(P::ratio, ParameterSpec::continuous("comp.ratio", "Ratio", 1.0f, 100.0f, 4.0f, ParamUnit::Ratio)
                      .withCentre(4.0f)
                      .withFormatter([](float v) { return v >= 99.5f ? juce::String("inf : 1") : juce::String(v, 2) + " : 1"; }),
        "Compressor");
    set(P::attack,
        ParameterSpec::continuous("comp.attack", "Attack", 0.01f, 300.0f, 10.0f, ParamUnit::Milliseconds).withCentre(10.0f),
        "Compressor");
    set(P::release,
        ParameterSpec::continuous("comp.release", "Release", 1.0f, 3000.0f, 150.0f, ParamUnit::Milliseconds).withCentre(150.0f),
        "Compressor");
    set(P::knee, ParameterSpec::continuous("comp.knee", "Knee", 0.0f, 24.0f, 6.0f, ParamUnit::Decibels), "Compressor");
    set(P::makeup, ParameterSpec::continuous("comp.makeup", "Makeup", -12.0f, 36.0f, 0.0f, ParamUnit::Decibels), "Compressor");
    set(P::mix, ParameterSpec::continuous("comp.mix", "Dry/Wet", 0.0f, 1.0f, 1.0f, ParamUnit::Percent).withFormatter(pct),
        "Compressor");
    set(P::detection, ParameterSpec::choice("comp.detection", "Detection", {"Peak", "RMS"}, 0), "Compressor");
    set(P::sidechainOn, ParameterSpec::toggle("comp.sidechain", "Sidechain", false), "Sidechain");
    set(P::sidechainGain,
        ParameterSpec::continuous("comp.sidechainGain", "Sidechain Gain", -24.0f, 24.0f, 0.0f, ParamUnit::Decibels), "Sidechain");
    set(P::eqOn, ParameterSpec::toggle("comp.eq", "EQ", false), "Sidechain");
    set(P::eqType, ParameterSpec::choice("comp.eqType", "EQ Type", {"Low-pass", "Band-pass", "High-pass", "Low shelf", "Bell", "High shelf"}, 1),
        "Sidechain");
    set(P::eqFrequency,
        ParameterSpec::continuous("comp.eqFrequency", "EQ Frequency", 20.0f, 20000.0f, 100.0f, ParamUnit::Hz).withCentre(1000.0f),
        "Sidechain");
    set(P::eqQ, ParameterSpec::continuous("comp.eqQ", "EQ Q", 0.1f, 18.0f, 0.71f).withCentre(1.0f), "Sidechain");
    set(P::eqGain, ParameterSpec::continuous("comp.eqGain", "EQ Gain", -15.0f, 15.0f, 0.0f, ParamUnit::Decibels), "Sidechain");
    return s;
}

dsp::BiquadType eqTypeFor(int index)
{
    switch (index)
    {
    case 0: return dsp::BiquadType::LowPass;
    case 2: return dsp::BiquadType::HighPass;
    case 3: return dsp::BiquadType::LowShelf;
    case 4: return dsp::BiquadType::Peak;
    case 5: return dsp::BiquadType::HighShelf;
    default: return dsp::BiquadType::BandPass;
    }
}
} // namespace

CompressorDevice::CompressorDevice() : StudioDevice(typeId, "Compressor", DeviceKind::AudioEffect, makeSpecs())
{
}

void CompressorDevice::prepare(double, int)
{
    lastEqType = -1;
    reset();
}

void CompressorDevice::reset() noexcept
{
    reduction = 0.0f;
    rmsState = 0.0f;
    sidechainEq[0].reset();
    sidechainEq[1].reset();
}

float CompressorDevice::staticCurve(float x) const noexcept
{
    const float t = value(P::threshold);
    const float r = value(P::ratio) >= 99.5f ? 1.0e6f : value(P::ratio);
    const float w = value(P::knee);
    const float over = x - t;
    if (2.0f * over < -w)
        return x;
    if (w > 0.0f && 2.0f * std::abs(over) <= w)
    {
        const float k = over + 0.5f * w;
        return x + (1.0f / r - 1.0f) * k * k / (2.0f * w);
    }
    return t + over / r;
}

void CompressorDevice::process(DeviceBuffers& io, const DeviceTimeInfo&) noexcept
{
    const int n = io.numSamples;
    float* l = io.channels[0];
    float* r = io.channels[1];
    const double sr = sampleRate();
    const auto attackCoef = static_cast<float>(std::exp(-1.0 / (std::max(0.01f, value(P::attack)) * 0.001 * sr)));
    const auto releaseCoef = static_cast<float>(std::exp(-1.0 / (std::max(1.0f, value(P::release)) * 0.001 * sr)));
    const auto rmsCoef = static_cast<float>(std::exp(-1.0 / (0.01 * sr))); // 10 ms RMS window
    const bool rms = juce::roundToInt(value(P::detection)) == 1;
    const bool useSidechain = value(P::sidechainOn) >= 0.5f && io.sidechain != nullptr;
    const float sidechainGain = std::pow(10.0f, value(P::sidechainGain) / 20.0f);
    const bool eq = value(P::eqOn) >= 0.5f;
    const float targetMakeup = std::pow(10.0f, value(P::makeup) / 20.0f);
    const float targetMix = value(P::mix);
    const float makeupStep = (targetMakeup - makeupGain) / static_cast<float>(n);
    const float mixStep = (targetMix - mixValue) / static_cast<float>(n);

    if (eq)
    {
        const int type = juce::roundToInt(value(P::eqType));
        const float f = value(P::eqFrequency);
        const float q = value(P::eqQ);
        const float g = value(P::eqGain);
        if (type != lastEqType || f != lastEqFrequency || q != lastEqQ || g != lastEqGain)
        {
            const auto c = dsp::BiquadCoefficients::design(eqTypeFor(type), f, q, g, sr);
            sidechainEq[0].set(c);
            sidechainEq[1].set(c);
            lastEqType = type;
            lastEqFrequency = f;
            lastEqQ = q;
            lastEqGain = g;
        }
    }

    float deepest = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        float dl = useSidechain ? io.sidechain[0][i] * sidechainGain : l[i];
        float dr = useSidechain ? io.sidechain[1][i] * sidechainGain : r[i];
        if (eq)
        {
            dl = sidechainEq[0].process(dl);
            dr = sidechainEq[1].process(dr);
        }
        float level = std::max(std::abs(dl), std::abs(dr));
        if (rms)
        {
            rmsState = level * level + rmsCoef * (rmsState - level * level);
            level = std::sqrt(std::max(rmsState, 0.0f));
        }
        const float inputDb = 20.0f * std::log10(std::max(level, 1.0e-9f));
        const float target = staticCurve(inputDb) - inputDb; // <= 0

        // Gain reduction follower: attack when reducing more, release when letting go.
        const float coef = target < reduction ? attackCoef : releaseCoef;
        reduction = target + coef * (reduction - target);
        deepest = std::min(deepest, reduction);

        makeupGain += makeupStep;
        mixValue += mixStep;
        const float g = std::pow(10.0f, reduction / 20.0f) * makeupGain;
        const float wet = mixValue;
        l[i] = l[i] * (1.0f - wet + wet * g);
        r[i] = r[i] * (1.0f - wet + wet * g);
    }
    reductionDb.store(deepest, std::memory_order_relaxed);
}

} // namespace lattice
