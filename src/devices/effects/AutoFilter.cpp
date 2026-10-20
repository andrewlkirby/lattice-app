#include "devices/effects/AutoFilter.h"

#include "devices/dsp/SyncRates.h"

#include <algorithm>
#include <cmath>

namespace lattice
{
namespace
{
using P = AutoFilterDevice::Param;

std::vector<ParameterSpec> makeSpecs()
{
    std::vector<ParameterSpec> s(static_cast<std::size_t>(P::numParams));
    const auto set = [&](int index, ParameterSpec spec, const char* group) {
        spec.group = group;
        s[static_cast<std::size_t>(index)] = std::move(spec);
    };
    const auto pct = [](float v) { return juce::String(juce::roundToInt(v * 100.0f)) + " %"; };
    const auto oct = [](float v) { return (v > 0.0f ? "+" : "") + juce::String(v, 2) + " oct"; };
    set(P::type,
        ParameterSpec::choice("af.type", "Type", {"Low-pass 12", "Low-pass 24", "High-pass 12", "High-pass 24", "Band-pass", "Notch", "Ladder"}, 1),
        "Filter");
    set(P::cutoff, ParameterSpec::continuous("af.cutoff", "Cutoff", 20.0f, 20000.0f, 1500.0f, ParamUnit::Hz).withCentre(1000.0f), "Filter");
    set(P::resonance, ParameterSpec::continuous("af.resonance", "Resonance", 0.0f, 1.0f, 0.3f, ParamUnit::Percent).withFormatter(pct), "Filter");
    set(P::drive, ParameterSpec::continuous("af.drive", "Drive", 0.0f, 24.0f, 0.0f, ParamUnit::Decibels), "Filter");
    set(P::lfoAmount, ParameterSpec::continuous("af.lfoAmount", "LFO Amount", 0.0f, 6.0f, 1.0f).withFormatter(oct), "LFO");
    set(P::lfoShape, ParameterSpec::choice("af.lfoShape", "Shape", {"Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H"}, 0), "LFO");
    set(P::lfoRate, ParameterSpec::continuous("af.lfoRate", "Rate", 0.01f, 40.0f, 1.0f, ParamUnit::Hz).withCentre(2.0f), "LFO");
    set(P::lfoSync, ParameterSpec::toggle("af.lfoSync", "Sync", false), "LFO");
    set(P::lfoSyncRate, ParameterSpec::choice("af.lfoSyncRate", "Synced Rate", dsp::SyncRates::names(), dsp::SyncRates::indexOf("1/4")), "LFO");
    set(P::lfoPhase, ParameterSpec::continuous("af.lfoPhase", "Stereo Phase", 0.0f, 360.0f, 0.0f).withFormatter([](float v) {
        return juce::String(juce::roundToInt(v)) + " deg";
    }), "LFO");
    set(P::envAmount, ParameterSpec::continuous("af.envAmount", "Env Amount", -6.0f, 6.0f, 0.0f).withFormatter(oct), "Envelope");
    set(P::envAttack, ParameterSpec::continuous("af.envAttack", "Attack", 0.1f, 300.0f, 6.0f, ParamUnit::Milliseconds).withCentre(20.0f), "Envelope");
    set(P::envRelease, ParameterSpec::continuous("af.envRelease", "Release", 1.0f, 3000.0f, 200.0f, ParamUnit::Milliseconds).withCentre(200.0f), "Envelope");
    set(P::mix, ParameterSpec::continuous("af.mix", "Dry/Wet", 0.0f, 1.0f, 1.0f, ParamUnit::Percent).withFormatter(pct), "Envelope");
    return s;
}
} // namespace

AutoFilterDevice::AutoFilterDevice() : StudioDevice(typeId, "Auto Filter", DeviceKind::AudioEffect, makeSpecs())
{
}

void AutoFilterDevice::prepare(double, int)
{
    reset();
}

void AutoFilterDevice::reset() noexcept
{
    for (int ch = 0; ch < 2; ++ch)
    {
        svf[ch].reset();
        ladder[ch].reset();
        g[ch] = -1.0f;
    }
    follower.reset();
    lfo.reset();
}

void AutoFilterDevice::process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept
{
    const int n = io.numSamples;
    const double sr = sampleRate();
    const int filterType = juce::roundToInt(value(P::type));
    const bool useLadder = filterType == 6;
    const bool steep = filterType == 1 || filterType == 3;
    dsp::SvfMode mode = dsp::SvfMode::LowPass;
    if (filterType == 2 || filterType == 3)
        mode = dsp::SvfMode::HighPass;
    else if (filterType == 4)
        mode = dsp::SvfMode::BandPass;
    else if (filterType == 5)
        mode = dsp::SvfMode::Notch;
    const float drive = std::pow(10.0f, value(P::drive) / 20.0f);
    const auto shape = static_cast<dsp::LfoShape>(juce::roundToInt(value(P::lfoShape)));
    const bool synced = value(P::lfoSync) >= 0.5f;
    const double beats = dsp::SyncRates::beatsAt(juce::roundToInt(value(P::lfoSyncRate)));
    const double stereoPhase = static_cast<double>(value(P::lfoPhase)) / 360.0;
    follower.setTimes(sr, value(P::envAttack) * 0.001, value(P::envRelease) * 0.001);
    const float targetMix = value(P::mix);
    const float mixStep = (targetMix - mixValue) / static_cast<float>(n);
    float* chans[2] = {io.channels[0], io.channels[1]};
    float dry[2][controlBlock];

    for (int start = 0; start < n; start += controlBlock)
    {
        const int m = std::min(controlBlock, n - start);
        // The envelope follows the louder channel of the input.
        float env = follower.value();
        for (int i = 0; i < m; ++i)
            env = follower.peak(std::max(std::abs(chans[0][start + i]), std::abs(chans[1][start + i])));
        double phase = 0.0;
        if (synced)
            phase = dsp::Lfo::phaseFromBeat(time.beatAt(start), beats);
        else
        {
            phase = lfo.currentPhase();
            lfo.setPhase(phase + static_cast<double>(value(P::lfoRate)) * m / sr);
        }

        for (int ch = 0; ch < 2; ++ch)
        {
            const double p = phase + (ch == 1 ? stereoPhase : 0.0);
            const float lfoValue = lfo.valueAt(shape, p - std::floor(p));
            const double octaves = value(P::lfoAmount) * 0.5 * lfoValue + value(P::envAmount) * std::min(1.0f, env * 2.0f);
            const double hz = std::clamp(static_cast<double>(value(P::cutoff)) * std::exp2(octaves), 20.0, 0.45 * sr);
            const float target = useLadder ? dsp::LadderFilter::prewarpOversampled(hz, sr) : dsp::SvfCoefficients::prewarp(hz, sr);
            if (g[ch] < 0.0f)
                g[ch] = target;
            const float mid = 0.5f * (g[ch] + target); // coefficients from the middle of the block
            float* x = chans[ch] + start;
            std::copy_n(x, m, dry[ch]);
            if (useLadder)
            {
                ladder[ch].setCoefficients(mid, value(P::resonance), drive);
                for (int i = 0; i < m; ++i)
                    x[i] = ladder[ch].process(x[i]);
            }
            else
            {
                svf[ch].setCoefficients(mid, value(P::resonance), steep);
                svf[ch].processBlock(x, m, drive, mode);
                if (drive > 1.0f)
                {
                    const float makeUp = 1.0f / std::sqrt(drive);
                    for (int i = 0; i < m; ++i)
                        x[i] = dsp::fastTanh(x[i]) * makeUp;
                }
            }
            g[ch] = target;
        }
        for (int i = 0; i < m; ++i)
        {
            mixValue += mixStep;
            for (int ch = 0; ch < 2; ++ch)
                chans[ch][start + i] = dry[ch][i] + mixValue * (chans[ch][start + i] - dry[ch][i]);
        }
    }
}

} // namespace lattice
