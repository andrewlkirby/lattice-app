#include "devices/effects/Delay.h"

#include "devices/dsp/SyncRates.h"

#include <algorithm>
#include <cmath>

namespace lattice
{
namespace
{
using P = DelayDevice::Param;

std::vector<ParameterSpec> makeSpecs()
{
    std::vector<ParameterSpec> s(static_cast<std::size_t>(P::numParams));
    const auto set = [&](int index, ParameterSpec spec, const char* group) {
        spec.group = group;
        s[static_cast<std::size_t>(index)] = std::move(spec);
    };
    const auto pct = [](float v) { return juce::String(juce::roundToInt(v * 100.0f)) + " %"; };
    set(P::mode, ParameterSpec::choice("delay.mode", "Mode", {"Stereo", "Ping-Pong"}, 0), "Delay");
    set(P::sync, ParameterSpec::toggle("delay.sync", "Sync", true), "Delay");
    set(P::syncLeft, ParameterSpec::choice("delay.syncLeft", "Time L", dsp::SyncRates::names(), dsp::SyncRates::indexOf("1/8")),
        "Delay");
    set(P::syncRight, ParameterSpec::choice("delay.syncRight", "Time R", dsp::SyncRates::names(), dsp::SyncRates::indexOf("1/8D")),
        "Delay");
    set(P::msLeft,
        ParameterSpec::continuous("delay.msLeft", "Free L", 1.0f, 5000.0f, 250.0f, ParamUnit::Milliseconds).withCentre(300.0f),
        "Delay");
    set(P::msRight,
        ParameterSpec::continuous("delay.msRight", "Free R", 1.0f, 5000.0f, 375.0f, ParamUnit::Milliseconds).withCentre(300.0f),
        "Delay");
    set(P::link, ParameterSpec::toggle("delay.link", "Link", false), "Delay");
    set(P::feedback,
        ParameterSpec::continuous("delay.feedback", "Feedback", 0.0f, 1.0f, 0.4f, ParamUnit::Percent).withFormatter(pct),
        "Delay");
    set(P::lowCut,
        ParameterSpec::continuous("delay.lowCut", "Low Cut", 20.0f, 2000.0f, 50.0f, ParamUnit::Hz).withCentre(200.0f),
        "Filter");
    set(P::highCut,
        ParameterSpec::continuous("delay.highCut", "High Cut", 500.0f, 20000.0f, 12000.0f, ParamUnit::Hz).withCentre(4000.0f),
        "Filter");
    set(P::modRate, ParameterSpec::continuous("delay.modRate", "Mod Rate", 0.01f, 10.0f, 0.5f, ParamUnit::Hz).withCentre(1.0f),
        "Modulation");
    set(P::modDepth,
        ParameterSpec::continuous("delay.modDepth", "Mod Depth", 0.0f, 10.0f, 0.0f, ParamUnit::Milliseconds), "Modulation");
    set(P::freeze, ParameterSpec::toggle("delay.freeze", "Freeze", false), "Modulation");
    set(P::mix, ParameterSpec::continuous("delay.mix", "Dry/Wet", 0.0f, 1.0f, 0.5f, ParamUnit::Percent).withFormatter(pct),
        "Modulation");
    return s;
}
} // namespace

DelayDevice::DelayDevice() : StudioDevice(typeId, "Delay", DeviceKind::AudioEffect, makeSpecs())
{
}

void DelayDevice::prepare(double sr, int)
{
    for (auto& line : lines)
        line.prepare(static_cast<int>(maxSeconds * sr) + 16);
    lastLowCut = lastHighCut = -1.0f;
    started = false;
    reset();
}

void DelayDevice::reset() noexcept
{
    for (auto& line : lines)
        line.reset();
    for (auto& f : lowCutFilter)
        f.reset();
    for (auto& f : highCutFilter)
        f.reset();
    lfoPhase = 0.0;
}

void DelayDevice::process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept
{
    const int n = io.numSamples;
    const double sr = sampleRate();
    const bool pingPong = juce::roundToInt(value(P::mode)) == 1;
    const bool freeze = value(P::freeze) >= 0.5f;
    const bool linked = value(P::link) >= 0.5f;

    // Target times in samples.
    double target[2];
    for (int ch = 0; ch < 2; ++ch)
    {
        const int side = linked ? 0 : ch;
        if (value(P::sync) >= 0.5f)
            target[ch] = dsp::SyncRates::beatsAt(juce::roundToInt(value(side == 0 ? P::syncLeft : P::syncRight))) *
                         time.samplesPerBeat;
        else
            target[ch] = static_cast<double>(value(side == 0 ? P::msLeft : P::msRight)) * 0.001 * sr;
        target[ch] = std::clamp(target[ch], 2.0, static_cast<double>(lines[ch].capacity() - 8));
    }
    if (!started)
    {
        delay[0] = target[0];
        delay[1] = target[1];
        mixValue = value(P::mix);
        started = true;
    }

    if (value(P::lowCut) != lastLowCut || value(P::highCut) != lastHighCut)
    {
        lastLowCut = value(P::lowCut);
        lastHighCut = value(P::highCut);
        const auto lc = dsp::BiquadCoefficients::design(dsp::BiquadType::HighPass, lastLowCut, 0.7071, 0.0, sr);
        const auto hc = dsp::BiquadCoefficients::design(dsp::BiquadType::LowPass, lastHighCut, 0.7071, 0.0, sr);
        for (int ch = 0; ch < 2; ++ch)
        {
            lowCutFilter[ch].set(lc);
            highCutFilter[ch].set(hc);
        }
    }

    const double glide = 1.0 - std::exp(-1.0 / (0.1 * sr)); // about 100 ms
    const double depth = static_cast<double>(value(P::modDepth)) * 0.001 * sr;
    const double lfoStep = static_cast<double>(value(P::modRate)) / sr;
    const float feedback = freeze ? 1.0f : value(P::feedback);
    const float targetMix = value(P::mix);
    const float mixStep = (targetMix - mixValue) / static_cast<float>(n);
    float* l = io.channels[0];
    float* r = io.channels[1];

    for (int i = 0; i < n; ++i)
    {
        float wet[2];
        for (int ch = 0; ch < 2; ++ch)
        {
            if (delay[ch] != target[ch])
            {
                delay[ch] += (target[ch] - delay[ch]) * glide;
                if (std::abs(target[ch] - delay[ch]) < 1e-6)
                    delay[ch] = target[ch];
            }
            double d = delay[ch];
            if (depth > 0.0)
                d += depth * (1.0 + std::sin(6.283185307179586 * (lfoPhase + 0.25 * ch))) * 0.5;
            // Read before this sample is written: d - 1 samples back is d samples of delay.
            wet[ch] = lines[ch].read(static_cast<float>(d - 1.0));
        }
        lfoPhase += lfoStep;
        if (lfoPhase >= 1.0)
            lfoPhase -= 1.0;

        float back[2];
        for (int ch = 0; ch < 2; ++ch)
        {
            float x = wet[ch] * feedback;
            if (!freeze)
                x = highCutFilter[ch].process(lowCutFilter[ch].process(x));
            back[ch] = x;
        }
        const float inL = freeze ? 0.0f : l[i];
        const float inR = freeze ? 0.0f : r[i];
        if (pingPong)
        {
            // The input goes into the left line; the lines feed each other.
            lines[0].push(0.5f * (inL + inR) + back[1]);
            lines[1].push(back[0]);
        }
        else
        {
            lines[0].push(inL + back[0]);
            lines[1].push(inR + back[1]);
        }

        mixValue += mixStep;
        l[i] = l[i] * (1.0f - mixValue) + wet[0] * mixValue;
        r[i] = r[i] * (1.0f - mixValue) + wet[1] * mixValue;
    }
}

} // namespace lattice
