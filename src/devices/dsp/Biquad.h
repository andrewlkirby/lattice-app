#pragma once

#include "core/RealtimeCheck.h"

#include <complex>
#include <cstdint>

namespace lattice::dsp
{

enum class BiquadType : std::uint8_t
{
    LowPass,
    HighPass,
    BandPass,
    Notch,
    Peak,
    LowShelf,
    HighShelf
};

/** Second-order section coefficients, normalised so a0 = 1:
    H(z) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2). */
struct BiquadCoefficients
{
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

    /** A design that matches the analog prototype's magnitude response instead of warping it near
        Nyquist (docs/04-m3-devices.md, "Filters"):
        - low-pass, high-pass, band-pass and peak: after Vicanek, "Matched Second Order Digital
          Filters" (2016): exact analog poles (impulse invariance), zeros fitted to the analog
          magnitude at DC, the centre frequency and Nyquist (the high-pass keeps its double zero
          at DC and fits the centre);
        - shelves and notch: the bilinear transform, prewarped at the centre frequency. Shelves are
          already exact at DC and Nyquist that way, and a notch at its centre.
        gainDb is used by peak and shelves. */
    static BiquadCoefficients design(BiquadType type, double frequency, double q, double gainDb,
                                     double sampleRate);

    /** The classic bilinear-transform design (RBJ cookbook), for every type. */
    static BiquadCoefficients designBilinear(BiquadType type, double frequency, double q, double gainDb,
                                             double sampleRate);

    /** The digital response at a frequency (for tests and displays). */
    std::complex<double> response(double frequency, double sampleRate) const;

    /** The analog prototype's response at a frequency (for tests). */
    static std::complex<double> analogResponse(BiquadType type, double frequency, double centre, double q,
                                               double gainDb);
};

/** Transposed direct form II: good numerical behaviour in floating point, two states. */
struct Biquad
{
    float b0 = 1.0f, b1 = 0.0f, b2 = 0.0f, a1 = 0.0f, a2 = 0.0f;
    float z1 = 0.0f, z2 = 0.0f;

    void set(const BiquadCoefficients& c) noexcept
    {
        b0 = static_cast<float>(c.b0);
        b1 = static_cast<float>(c.b1);
        b2 = static_cast<float>(c.b2);
        a1 = static_cast<float>(c.a1);
        a2 = static_cast<float>(c.a2);
    }
    void reset() noexcept { z1 = z2 = 0.0f; }

    float process(float x) noexcept LATTICE_NONBLOCKING
    {
        const float y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

} // namespace lattice::dsp
