#include "devices/effects/EqEight.h"

#include <algorithm>
#include <cmath>

namespace lattice
{
namespace
{
enum Field
{
    fieldOn,
    fieldShape,
    fieldFrequency,
    fieldGain,
    fieldQ,
    fieldTarget
};

std::vector<ParameterSpec> makeSpecs()
{
    std::vector<ParameterSpec> s;
    const juce::StringArray shapes{"Low Cut 48", "Low Cut 12", "Low Shelf", "Bell", "Notch", "High Shelf", "High Cut 12",
                                   "High Cut 48"};
    // Live's default layout: a low cut, three bells, a high shelf ... spread over the spectrum.
    constexpr float defaultFrequencies[EqEightDevice::numBands] = {30.0f, 100.0f, 250.0f, 600.0f,
                                                                   1500.0f, 3500.0f, 8000.0f, 16000.0f};
    for (int b = 0; b < EqEightDevice::numBands; ++b)
    {
        const juce::String n(b + 1);
        const juce::String group = "Band " + n;
        const int defaultShape = b == 0 ? 1 : (b == EqEightDevice::numBands - 1 ? 5 : 3);
        s.push_back(ParameterSpec::toggle("band" + n + ".on", "On " + n, b == 0 || b == 7 ? false : true).inGroup(group));
        s.push_back(ParameterSpec::choice("band" + n + ".shape", "Shape " + n, shapes, defaultShape).inGroup(group));
        s.push_back(ParameterSpec::continuous("band" + n + ".frequency", "Freq " + n, 10.0f, 22000.0f,
                                              defaultFrequencies[b], ParamUnit::Hz)
                        .withCentre(1000.0f)
                        .inGroup(group));
        s.push_back(ParameterSpec::continuous("band" + n + ".gain", "Gain " + n, -15.0f, 15.0f, 0.0f, ParamUnit::Decibels)
                        .inGroup(group));
        s.push_back(ParameterSpec::continuous("band" + n + ".q", "Q " + n, 0.1f, 18.0f, 0.71f).withCentre(1.0f).inGroup(group));
        s.push_back(ParameterSpec::choice("band" + n + ".target", "Channel " + n, {"Stereo", "Mid", "Side"}, 0).inGroup(group));
    }
    s.push_back(ParameterSpec::continuous("eq.output", "Output", -12.0f, 12.0f, 0.0f, ParamUnit::Decibels).inGroup("Output"));
    s.push_back(ParameterSpec::continuous("eq.scale", "Scale", -2.0f, 2.0f, 1.0f, ParamUnit::Percent)
                    .withFormatter([](float v) { return juce::String(juce::roundToInt(v * 100.0f)) + " %"; })
                    .inGroup("Output"));
    return s;
}

/** Butterworth Q values for a 48 dB (8th-order) response, one per section. */
constexpr double butterworth8[4] = {0.50979558, 0.60134489, 0.89997622, 2.56291545};
} // namespace

EqEightDevice::EqEightDevice() : StudioDevice(typeId, "EQ Eight", DeviceKind::AudioEffect, makeSpecs())
{
}

int EqEightDevice::designBand(Shape shape, double frequency, double gainDb, double q, double sr,
                              std::array<dsp::BiquadCoefficients, sectionsPerBand>& out)
{
    using T = dsp::BiquadType;
    switch (shape)
    {
    case Shape::LowCut48:
    case Shape::HighCut48:
        for (int i = 0; i < 4; ++i)
        {
            // Q shapes the resonance of the last (most resonant) section.
            const double sectionQ = i == 3 ? butterworth8[3] * q / 0.7071 : butterworth8[i];
            out[static_cast<std::size_t>(i)] = dsp::BiquadCoefficients::design(
                shape == Shape::LowCut48 ? T::HighPass : T::LowPass, frequency, sectionQ, 0.0, sr);
        }
        return 4;
    case Shape::LowCut12:
        out[0] = dsp::BiquadCoefficients::design(T::HighPass, frequency, q, 0.0, sr);
        return 1;
    case Shape::HighCut12:
        out[0] = dsp::BiquadCoefficients::design(T::LowPass, frequency, q, 0.0, sr);
        return 1;
    case Shape::LowShelf:
        out[0] = dsp::BiquadCoefficients::design(T::LowShelf, frequency, q, gainDb, sr);
        return 1;
    case Shape::HighShelf:
        out[0] = dsp::BiquadCoefficients::design(T::HighShelf, frequency, q, gainDb, sr);
        return 1;
    case Shape::Notch:
        out[0] = dsp::BiquadCoefficients::design(T::Notch, frequency, q, 0.0, sr);
        return 1;
    case Shape::Bell:
        out[0] = dsp::BiquadCoefficients::design(T::Peak, frequency, q, gainDb, sr);
        return 1;
    }
    return 0;
}

void EqEightDevice::prepare(double, int)
{
    for (auto& b : bands)
        b.started = false;
    reset();
}

void EqEightDevice::reset() noexcept
{
    for (auto& b : bands)
        for (auto& channel : b.filters)
            for (auto& f : channel)
                f.reset();
}

void EqEightDevice::updateBand(int index, bool snap) noexcept
{
    auto& b = bands[static_cast<std::size_t>(index)];
    const bool on = value(paramIndex(index, fieldOn)) >= 0.5f;
    const auto shape = static_cast<Shape>(juce::roundToInt(value(paramIndex(index, fieldShape))));
    if (on != b.on || shape != b.shape)
    {
        b.sections = 0;
        // Switching a band or its shape starts its filters afresh (the base class crossfades bypass).
        b.on = on;
        b.shape = shape;
        for (auto& channel : b.filters)
            for (auto& f : channel)
                f.reset();
        snap = true;
    }
    b.target = static_cast<Target>(juce::roundToInt(value(paramIndex(index, fieldTarget))));
    if (!b.on)
        return;

    const float frequency = value(paramIndex(index, fieldFrequency));
    const float gain = value(paramIndex(index, fieldGain)) * value(scaleParam);
    const float q = value(paramIndex(index, fieldQ));
    if (snap || !b.started)
    {
        b.frequency = frequency;
        b.gain = gain;
        b.q = q;
        b.started = true;
    }
    else
    {
        const bool there = std::abs(frequency / b.frequency - 1.0f) < 1.0e-5f && std::abs(q / b.q - 1.0f) < 1.0e-5f &&
                           std::abs(gain - b.gain) < 1.0e-4f;
        if (there)
        {
            if (b.frequency == frequency && b.q == q && b.gain == gain && b.sections > 0)
                return; // settled: the coefficients are current
            b.frequency = frequency;
            b.q = q;
            b.gain = gain;
        }
        else
        {
            // Glide over about 20 ms: frequency and Q in log terms, gain in dB.
            const auto k = static_cast<float>(1.0 - std::exp(-static_cast<double>(controlBlock) / (0.02 * sampleRate())));
            b.frequency *= std::pow(frequency / b.frequency, k);
            b.q *= std::pow(q / b.q, k);
            b.gain += k * (gain - b.gain);
        }
    }
    b.sections = designBand(b.shape, b.frequency, b.gain, b.q, sampleRate(), b.coefficients);
    for (auto& channel : b.filters)
        for (int s = 0; s < b.sections; ++s)
            channel[static_cast<std::size_t>(s)].set(b.coefficients[static_cast<std::size_t>(s)]);
}

std::complex<double> EqEightDevice::response(double frequency, Target channelTarget) const
{
    std::complex<double> h = std::pow(10.0, value(outputGainParam) / 20.0);
    for (int i = 0; i < numBands; ++i)
    {
        const bool on = value(paramIndex(i, fieldOn)) >= 0.5f;
        const auto target = static_cast<Target>(juce::roundToInt(value(paramIndex(i, fieldTarget))));
        if (!on || (target != Target::Stereo && target != channelTarget))
            continue;
        std::array<dsp::BiquadCoefficients, sectionsPerBand> c{};
        const int n = designBand(static_cast<Shape>(juce::roundToInt(value(paramIndex(i, fieldShape)))),
                                 value(paramIndex(i, fieldFrequency)),
                                 value(paramIndex(i, fieldGain)) * value(scaleParam), value(paramIndex(i, fieldQ)),
                                 sampleRate(), c);
        for (int s = 0; s < n; ++s)
            h *= c[static_cast<std::size_t>(s)].response(frequency, sampleRate());
    }
    return h;
}

void EqEightDevice::process(DeviceBuffers& io, const DeviceTimeInfo&) noexcept
{
    const int n = io.numSamples;
    float* l = io.channels[0];
    float* r = io.channels[1];
    bool midSide = false;
    for (int i = 0; i < numBands; ++i)
        midSide = midSide || (value(paramIndex(i, fieldOn)) >= 0.5f &&
                              juce::roundToInt(value(paramIndex(i, fieldTarget))) != static_cast<int>(Target::Stereo));
    if (midSide)
        for (int i = 0; i < n; ++i)
        {
            const float m = 0.5f * (l[i] + r[i]);
            const float s = 0.5f * (l[i] - r[i]);
            l[i] = m;
            r[i] = s;
        }

    for (int start = 0; start < n; start += controlBlock)
    {
        const int m = std::min(controlBlock, n - start);
        for (int b = 0; b < numBands; ++b)
        {
            updateBand(b, false);
            auto& band = bands[static_cast<std::size_t>(b)];
            if (!band.on)
                continue;
            const bool both = !midSide || band.target == Target::Stereo;
            if (both)
            {
                // Both channels in one loop: two independent filter chains, so the processor
                // overlaps them (a biquad is bound by its own feedback latency).
                float* xl = io.channels[0] + start;
                float* xr = io.channels[1] + start;
                for (int s = 0; s < band.sections; ++s)
                {
                    auto& fl = band.filters[0][static_cast<std::size_t>(s)];
                    auto& fr = band.filters[1][static_cast<std::size_t>(s)];
                    for (int i = 0; i < m; ++i)
                    {
                        xl[i] = fl.process(xl[i]);
                        xr[i] = fr.process(xr[i]);
                    }
                }
                continue;
            }
            const int ch = band.target == Target::Mid ? 0 : 1;
            float* x = io.channels[ch] + start;
            for (int s = 0; s < band.sections; ++s)
            {
                auto& f = band.filters[static_cast<std::size_t>(ch)][static_cast<std::size_t>(s)];
                for (int i = 0; i < m; ++i)
                    x[i] = f.process(x[i]);
            }
        }
    }

    if (midSide)
        for (int i = 0; i < n; ++i)
        {
            const float m = l[i];
            const float s = r[i];
            l[i] = m + s;
            r[i] = m - s;
        }

    const float target = std::pow(10.0f, value(outputGainParam) / 20.0f);
    const float step = (target - outputGain) / static_cast<float>(n);
    std::array<float, 256> mono{};
    int filled = 0;
    for (int i = 0; i < n; ++i)
    {
        outputGain += step;
        l[i] *= outputGain;
        r[i] *= outputGain;
        mono[static_cast<std::size_t>(filled++)] = 0.5f * (l[i] + r[i]);
        if (filled == static_cast<int>(mono.size()))
        {
            analyzerFifo.pushMany(mono.data(), filled);
            filled = 0;
        }
    }
    if (filled > 0)
        analyzerFifo.pushMany(mono.data(), filled);
}

} // namespace lattice
