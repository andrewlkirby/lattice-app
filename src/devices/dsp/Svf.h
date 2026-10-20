#pragma once

#include "core/RealtimeCheck.h"
#include "devices/dsp/VoiceFilterKernels.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lattice::dsp
{

/** Coefficients of a topology-preserving-transform state-variable filter (Zavalishin, "The Art of
    VA Filter Design"; the form used by Simper). Cutoff is prewarped, so the analog response is hit
    exactly at the cutoff; the filter stays stable under any modulation. */
struct SvfCoefficients
{
    float g = 0.0f; // tan(pi fc / fs)
    float k = 1.41421356f; // 1 / Q
    float a1 = 0.0f;
    float a2 = 0.0f;
    float a3 = 0.0f;

    static float prewarp(double cutoffHz, double sampleRate) noexcept
    {
        const double fc = std::clamp(cutoffHz, 1.0, sampleRate * 0.49);
        return static_cast<float>(std::tan(3.14159265358979323846 * fc / sampleRate));
    }

    void set(float gValue, float kValue) noexcept
    {
        g = gValue;
        k = kValue;
        a1 = 1.0f / (1.0f + g * (g + k));
        a2 = g * a1;
        a3 = g * a2;
    }

    /** Resonance 0..1 to 1/Q: Q from 0.707 (no peak) to about 47 (close to self-oscillation). */
    static float dampingFor(float resonance) noexcept
    {
        return 1.41421356f * (1.0f - 0.985f * std::clamp(resonance, 0.0f, 1.0f));
    }
};

/** One 12 dB/octave TPT SVF stage (its state is plain data, which the voice filter kernels run
    on: VoiceFilterKernels.h). */
struct SvfStage : SvfStageState
{
    SvfStage() noexcept : SvfStageState{0.0f, 0.0f} {}

    void reset() noexcept
    {
        ic1 = 0.0f;
        ic2 = 0.0f;
    }

    float process(float x, const SvfCoefficients& c, SvfMode mode) noexcept LATTICE_NONBLOCKING
    {
        const float v3 = x - ic2;
        const float v1 = c.a1 * ic1 + c.a2 * v3;
        const float v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        switch (mode)
        {
        case SvfMode::LowPass:
            return v2;
        case SvfMode::HighPass:
            return x - c.k * v1 - v2;
        case SvfMode::BandPass:
            return v1;
        case SvfMode::Notch:
            return x - c.k * v1;
        }
        return v2;
    }
};

/** A 12 or 24 dB/octave SVF. At 24 dB the first stage is fixed at Q = 0.541 and the second carries
    the resonance (Q = 1.307 at resonance 0), so resonance 0 is a 4th-order Butterworth response. */
class Svf
{
public:
    void reset() noexcept
    {
        first.reset();
        second.reset();
    }

    /** Sets the coefficients for both stages (call per sample or per control block). */
    void setCoefficients(float g, float resonance, bool steep) noexcept
    {
        isSteep = steep;
        if (steep)
        {
            const float q2 = 1.30656296f * std::pow(36.0f, std::clamp(resonance, 0.0f, 1.0f));
            c1.set(g, 1.0f / 0.54119610f);
            c2.set(g, 1.0f / q2);
        }
        else
        {
            c2.set(g, SvfCoefficients::dampingFor(resonance));
        }
    }

    float process(float x, SvfMode mode) noexcept LATTICE_NONBLOCKING
    {
        if (isSteep)
            x = first.process(x, c1, mode);
        return second.process(x, c2, mode);
    }

    /** The two stages' state (the 12 dB filter uses only the second). */
    SvfStage& stage(int index) noexcept { return index == 0 ? first : second; }

    /** A block in place, with an input gain; the mode is chosen once, outside the loop. */
    void processBlock(float* x, int n, float inputGain, SvfMode mode) noexcept LATTICE_NONBLOCKING
    {
        switch (mode)
        {
        case SvfMode::LowPass: run<SvfMode::LowPass>(x, n, inputGain); break;
        case SvfMode::HighPass: run<SvfMode::HighPass>(x, n, inputGain); break;
        case SvfMode::BandPass: run<SvfMode::BandPass>(x, n, inputGain); break;
        case SvfMode::Notch: run<SvfMode::Notch>(x, n, inputGain); break;
        }
    }

private:
    template <SvfMode mode>
    static float tick(float x, float& ic1, float& ic2, const SvfCoefficients& c) noexcept
    {
        const float v3 = x - ic2;
        const float v1 = c.a1 * ic1 + c.a2 * v3;
        const float v2 = ic2 + c.a2 * ic1 + c.a3 * v3;
        ic1 = 2.0f * v1 - ic1;
        ic2 = 2.0f * v2 - ic2;
        if constexpr (mode == SvfMode::LowPass)
            return v2;
        else if constexpr (mode == SvfMode::HighPass)
            return x - c.k * v1 - v2;
        else if constexpr (mode == SvfMode::BandPass)
            return v1;
        else
            return x - c.k * v1;
    }

    template <SvfMode mode>
    void run(float* x, int n, float inputGain) noexcept
    {
        float a1 = first.ic1, a2 = first.ic2, b1 = second.ic1, b2 = second.ic2;
        if (isSteep)
            for (int i = 0; i < n; ++i)
                x[i] = tick<mode>(tick<mode>(x[i] * inputGain, a1, a2, c1), b1, b2, c2);
        else
            for (int i = 0; i < n; ++i)
                x[i] = tick<mode>(x[i] * inputGain, b1, b2, c2);
        first.ic1 = a1;
        first.ic2 = a2;
        second.ic1 = b1;
        second.ic2 = b2;
    }

    SvfStage first;
    SvfStage second;
    SvfCoefficients c1;
    SvfCoefficients c2;
    bool isSteep = false;
};

} // namespace lattice::dsp
