#include "devices/effects/Chorus.h"

#include <algorithm>
#include <cmath>

namespace lattice
{
namespace
{
using P = ChorusDevice::Param;

std::vector<ParameterSpec> makeSpecs()
{
    std::vector<ParameterSpec> s(static_cast<std::size_t>(P::numParams));
    const auto set = [&](int index, ParameterSpec spec) {
        spec.group = "Chorus";
        s[static_cast<std::size_t>(index)] = std::move(spec);
    };
    const auto pct = [](float v) { return juce::String(juce::roundToInt(v * 100.0f)) + " %"; };
    set(P::voices, ParameterSpec::choice("chorus.voices", "Voices", {"2", "3"}, 0));
    set(P::rate, ParameterSpec::continuous("chorus.rate", "Rate", 0.02f, 10.0f, 0.6f, ParamUnit::Hz).withCentre(1.0f));
    set(P::depth, ParameterSpec::continuous("chorus.depth", "Depth", 0.0f, 10.0f, 2.5f, ParamUnit::Milliseconds));
    set(P::delay, ParameterSpec::continuous("chorus.delay", "Delay", 1.0f, 40.0f, 12.0f, ParamUnit::Milliseconds));
    set(P::feedback, ParameterSpec::continuous("chorus.feedback", "Feedback", -0.9f, 0.9f, 0.0f, ParamUnit::Percent).withFormatter(pct));
    set(P::width, ParameterSpec::continuous("chorus.width", "Width", 0.0f, 1.0f, 1.0f, ParamUnit::Percent).withFormatter(pct));
    set(P::mix, ParameterSpec::continuous("chorus.mix", "Dry/Wet", 0.0f, 1.0f, 0.5f, ParamUnit::Percent).withFormatter(pct));
    return s;
}
} // namespace

ChorusDevice::ChorusDevice() : StudioDevice(typeId, "Chorus", DeviceKind::AudioEffect, makeSpecs())
{
}

void ChorusDevice::prepare(double sr, int)
{
    for (auto& line : lines)
        line.prepare(static_cast<int>(0.06 * sr) + 8); // base delay + depth, 60 ms at most
    reset();
}

void ChorusDevice::reset() noexcept
{
    for (auto& line : lines)
        line.reset();
    feedbackState[0] = feedbackState[1] = 0.0f;
    phase = 0.0;
}

void ChorusDevice::process(DeviceBuffers& io, const DeviceTimeInfo&) noexcept
{
    const int n = io.numSamples;
    const double sr = sampleRate();
    const int numVoices = juce::roundToInt(value(P::voices)) + 2;
    const double base = static_cast<double>(value(P::delay)) * 0.001 * sr;
    const double depth = static_cast<double>(value(P::depth)) * 0.001 * sr;
    const double step = static_cast<double>(value(P::rate)) / sr;
    const float feedback = value(P::feedback);
    const float width = value(P::width);
    const float targetMix = value(P::mix);
    const float mixStep = (targetMix - mixValue) / static_cast<float>(n);
    float* l = io.channels[0];
    float* r = io.channels[1];

    // Voice pans: spread from -width to +width.
    float panL[3];
    float panR[3];
    for (int v = 0; v < numVoices; ++v)
    {
        const float x = (2.0f * static_cast<float>(v) / static_cast<float>(numVoices - 1) - 1.0f) * width;
        const float angle = (x + 1.0f) * 0.25f * 3.14159265f;
        panL[v] = std::cos(angle);
        panR[v] = std::sin(angle);
    }
    const float voiceGain = 1.41421356f / static_cast<float>(numVoices);

    for (int i = 0; i < n; ++i)
    {
        lines[0].push(l[i] + feedbackState[0] * feedback);
        lines[1].push(r[i] + feedbackState[1] * feedback);
        float wetL = 0.0f;
        float wetR = 0.0f;
        for (int v = 0; v < numVoices; ++v)
        {
            const double p = phase + static_cast<double>(v) / numVoices;
            const double d = base + depth * 0.5 * (1.0 + std::sin(6.283185307179586 * p));
            // Each voice reads the side it is panned towards most.
            const float a = lines[0].read(static_cast<float>(d));
            const float b = lines[1].read(static_cast<float>(d));
            const float mono = 0.5f * (a + b);
            wetL += (panL[v] * (a * (1.0f - width) + mono * width)) * voiceGain;
            wetR += (panR[v] * (b * (1.0f - width) + mono * width)) * voiceGain;
        }
        feedbackState[0] = wetL;
        feedbackState[1] = wetR;
        phase += step;
        if (phase >= 1.0)
            phase -= 1.0;
        mixValue += mixStep;
        l[i] = l[i] * (1.0f - mixValue) + wetL * mixValue;
        r[i] = r[i] * (1.0f - mixValue) + wetR * mixValue;
    }
}

} // namespace lattice
