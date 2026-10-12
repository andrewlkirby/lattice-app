#pragma once

#include "core/RealtimeCheck.h"
#include "devices/dsp/HalfBand.h"

#include <algorithm>
#include <cmath>

namespace lattice::dsp
{

/** A fast tanh: a rational approximation, exact at 0, within 0.2% everywhere, saturating at +-1. */
inline float fastTanh(float x) noexcept
{
    const float c = std::clamp(x, -3.0f, 3.0f);
    const float c2 = c * c;
    return c * (27.0f + c2) / (27.0f + 9.0f * c2);
}

/** A four-pole transistor-ladder low-pass with a saturating input stage (docs/04-m3-devices.md,
    "Filters").

    Zero-delay feedback: each sample solves the loop of four TPT one-poles exactly, with the
    saturation's gain taken from the previous sample (the "cheap non-linear" method), so cutoff
    and resonance are accurate and the filter is unconditionally stable, self-oscillating cleanly
    at full resonance. Runs at twice the sample rate through a half-band filter, which keeps the
    saturation's aliasing down. */
class LadderFilter
{
public:
    void reset() noexcept
    {
        s1 = s2 = s3 = s4 = 0.0f;
        lastArgument = 0.0f;
        oversampler.reset();
    }

    /** g: tan(pi fc / (2 fs)) for the oversampled rate (see prewarpOversampled). resonance 0..1
        (self-oscillation from about 0.97). drive >= 1 pushes the input stage into saturation. */
    void setCoefficients(float g, float resonance, float drive) noexcept
    {
        G = g / (1.0f + g);
        beta = 1.0f / (1.0f + g);
        k = 4.1f * std::clamp(resonance, 0.0f, 1.0f);
        inputGain = std::max(drive, 0.01f);
        outputGain = (1.0f + 0.5f * k) / std::sqrt(inputGain);
    }

    static float prewarpOversampled(double cutoffHz, double sampleRate) noexcept
    {
        const double fs = 2.0 * sampleRate;
        const double fc = std::clamp(cutoffHz, 1.0, sampleRate * 0.49); // up to the base rate's Nyquist
        return static_cast<float>(std::tan(3.14159265358979323846 * fc / fs));
    }

    float process(float x) noexcept LATTICE_NONBLOCKING
    {
        float a = 0.0f;
        float b = 0.0f;
        oversampler.upsample(x * inputGain, a, b);
        const float ya = tick(a);
        const float yb = tick(b);
        return oversampler.downsample(ya, yb) * outputGain;
    }

private:
    float tick(float u) noexcept LATTICE_NONBLOCKING
    {
        // Linearise the input saturation around the previous sample: tanh(a) = t a.
        const float t = std::abs(lastArgument) < 1.0e-4f ? 1.0f : fastTanh(lastArgument) / lastArgument;
        const float G2 = G * G;
        const float G4 = G2 * G2;
        const float S = beta * (G2 * G * s1 + G2 * s2 + G * s3 + s4);
        const float y4 = (G4 * t * u + S) / (1.0f + k * G4 * t);
        const float argument = u - k * y4;
        lastArgument = argument;
        const float x0 = t * argument;

        float v = (x0 - s1) * G;
        const float y1 = v + s1;
        s1 = y1 + v;
        v = (y1 - s2) * G;
        const float y2 = v + s2;
        s2 = y2 + v;
        v = (y2 - s3) * G;
        const float y3 = v + s3;
        s3 = y3 + v;
        v = (y3 - s4) * G;
        const float out = v + s4;
        s4 = out + v;
        return out;
    }

    HalfBand2x oversampler;
    float s1 = 0.0f, s2 = 0.0f, s3 = 0.0f, s4 = 0.0f;
    float G = 0.0f, beta = 1.0f, k = 0.0f;
    float inputGain = 1.0f, outputGain = 1.0f;
    float lastArgument = 0.0f;
};

} // namespace lattice::dsp
