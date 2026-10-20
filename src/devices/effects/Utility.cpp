#include "devices/effects/Utility.h"

#include <cmath>

namespace lattice
{
namespace
{
using P = UtilityDevice::Param;

std::vector<ParameterSpec> makeSpecs()
{
    std::vector<ParameterSpec> s(static_cast<std::size_t>(P::numParams));
    const auto set = [&](int index, ParameterSpec spec) {
        spec.group = "Utility";
        s[static_cast<std::size_t>(index)] = std::move(spec);
    };
    set(P::gain, ParameterSpec::continuous("utility.gain", "Gain", -70.0f, 35.0f, 0.0f, ParamUnit::Decibels).withCentre(0.0f));
    set(P::balance, ParameterSpec::continuous("utility.balance", "Balance", -1.0f, 1.0f, 0.0f).withFormatter([](float v) {
        const int p = juce::roundToInt(std::abs(v) * 50.0f);
        return p == 0 ? juce::String("C") : juce::String(p) + (v < 0.0f ? "L" : "R");
    }));
    set(P::width, ParameterSpec::continuous("utility.width", "Width", 0.0f, 4.0f, 1.0f, ParamUnit::Percent)
                      .withCentre(1.0f)
                      .withFormatter([](float v) { return juce::String(juce::roundToInt(v * 100.0f)) + " %"; }));
    set(P::mono, ParameterSpec::toggle("utility.mono", "Mono", false));
    set(P::invertLeft, ParameterSpec::toggle("utility.invertLeft", "Invert L", false));
    set(P::invertRight, ParameterSpec::toggle("utility.invertRight", "Invert R", false));
    set(P::dcFilter, ParameterSpec::toggle("utility.dc", "DC Filter", false));
    return s;
}
} // namespace

UtilityDevice::UtilityDevice() : StudioDevice(typeId, "Utility", DeviceKind::AudioEffect, makeSpecs())
{
}

void UtilityDevice::prepare(double sr, int)
{
    rampSamples = std::max(1, static_cast<int>(0.02 * sr));
    dcCoefficient = static_cast<float>(std::exp(-2.0 * 3.14159265358979 * 5.0 / sr)); // 5 Hz high-pass
    reset();
}

void UtilityDevice::reset() noexcept
{
    dcIn[0] = dcIn[1] = dcOut[0] = dcOut[1] = 0.0f;
    started = false;
}

UtilityDevice::Matrix UtilityDevice::currentMatrix() const noexcept
{
    const float db = value(P::gain);
    const float g = db <= -69.95f ? 0.0f : std::pow(10.0f, db / 20.0f);
    // Width through mid/side: L = M + wS, R = M - wS with M = (L + R) / 2, S = (L - R) / 2.
    const float w = value(P::mono) >= 0.5f ? 0.0f : value(P::width);
    Matrix m;
    m.ll = 0.5f * (1.0f + w);
    m.lr = 0.5f * (1.0f - w);
    m.rl = 0.5f * (1.0f - w);
    m.rr = 0.5f * (1.0f + w);
    // Balance: turning towards one side lowers the other (linearly, as in Live).
    const float b = value(P::balance);
    const float bl = std::min(1.0f, 1.0f - b);
    const float br = std::min(1.0f, 1.0f + b);
    const float il = value(P::invertLeft) >= 0.5f ? -1.0f : 1.0f;
    const float ir = value(P::invertRight) >= 0.5f ? -1.0f : 1.0f;
    m.ll *= g * bl * il;
    m.lr *= g * bl * il;
    m.rl *= g * br * ir;
    m.rr *= g * br * ir;
    return m;
}

void UtilityDevice::process(DeviceBuffers& io, const DeviceTimeInfo&) noexcept
{
    const int n = io.numSamples;
    float* l = io.channels[0];
    float* r = io.channels[1];
    const Matrix wanted = currentMatrix();
    if (!started)
    {
        matrix = target = wanted;
        remaining = 0;
        started = true;
    }
    else if (wanted.ll != target.ll || wanted.lr != target.lr || wanted.rl != target.rl || wanted.rr != target.rr)
    {
        // A new setting: ramp there over 20 ms, whatever the size of the change.
        target = wanted;
        const auto k = static_cast<float>(rampSamples);
        step = {(target.ll - matrix.ll) / k, (target.lr - matrix.lr) / k, (target.rl - matrix.rl) / k,
                (target.rr - matrix.rr) / k};
        remaining = rampSamples;
    }

    if (value(P::dcFilter) >= 0.5f)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            float* x = io.channels[ch];
            for (int i = 0; i < n; ++i)
            {
                // y = x - x[-1] + c y[-1]
                const float y = x[i] - dcIn[ch] + dcCoefficient * dcOut[ch];
                dcIn[ch] = x[i];
                dcOut[ch] = y;
                x[i] = y;
            }
        }
    }

    int i = 0;
    for (; i < n && remaining > 0; ++i, --remaining)
    {
        matrix.ll += step.ll;
        matrix.lr += step.lr;
        matrix.rl += step.rl;
        matrix.rr += step.rr;
        if (remaining == 1)
            matrix = target; // land exactly
        const float a = l[i];
        const float b = r[i];
        l[i] = matrix.ll * a + matrix.lr * b;
        r[i] = matrix.rl * a + matrix.rr * b;
    }
    for (; i < n; ++i)
    {
        const float a = l[i];
        const float b = r[i];
        l[i] = matrix.ll * a + matrix.lr * b;
        r[i] = matrix.rl * a + matrix.rr * b;
    }
}

} // namespace lattice
