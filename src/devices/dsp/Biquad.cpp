#include "devices/dsp/Biquad.h"

#include <algorithm>
#include <cmath>

namespace lattice::dsp
{
namespace
{
constexpr double pi = 3.14159265358979323846;

struct Poles
{
    double a1 = 0.0;
    double a2 = 0.0;
};

/** The analog poles of s^2 + s/q + 1 (at w0), mapped exactly: z = e^(s T). */
Poles matchedPoles(double w0, double q)
{
    const double zeta = 0.5 / q;
    Poles p;
    if (zeta <= 1.0)
        p.a1 = -2.0 * std::exp(-zeta * w0) * std::cos(std::sqrt(1.0 - zeta * zeta) * w0);
    else
        p.a1 = -2.0 * std::exp(-zeta * w0) * std::cosh(std::sqrt(zeta * zeta - 1.0) * w0);
    p.a2 = std::exp(-2.0 * zeta * w0);
    return p;
}

/** |1 + a1 z^-1 + a2 z^-2|^2 at w, written as A0 phi0 + A1 phi1 + A2 phi2 (Vicanek). */
struct PowerTerms
{
    double phi0, phi1, phi2;
    explicit PowerTerms(double w)
    {
        const double s = std::sin(0.5 * w);
        phi1 = s * s;
        phi0 = 1.0 - phi1;
        phi2 = 4.0 * phi0 * phi1;
    }
};
} // namespace

std::complex<double> BiquadCoefficients::analogResponse(BiquadType type, double frequency, double centre, double q,
                                                        double gainDb)
{
    const std::complex<double> s(0.0, frequency / centre);
    const auto s2 = s * s;
    const double a = std::pow(10.0, gainDb / 40.0);
    const double sqrtA = std::sqrt(a);
    switch (type)
    {
    case BiquadType::LowPass:
        return 1.0 / (s2 + s / q + 1.0);
    case BiquadType::HighPass:
        return s2 / (s2 + s / q + 1.0);
    case BiquadType::BandPass:
        return (s / q) / (s2 + s / q + 1.0);
    case BiquadType::Notch:
        return (s2 + 1.0) / (s2 + s / q + 1.0);
    case BiquadType::Peak:
        return (s2 + s * (a / q) + 1.0) / (s2 + s / (a * q) + 1.0);
    case BiquadType::LowShelf:
        return a * (s2 + (sqrtA / q) * s + a) / (a * s2 + (sqrtA / q) * s + 1.0);
    case BiquadType::HighShelf:
        return a * (a * s2 + (sqrtA / q) * s + 1.0) / (s2 + (sqrtA / q) * s + a);
    }
    return 1.0;
}

BiquadCoefficients BiquadCoefficients::designBilinear(BiquadType type, double frequency, double q, double gainDb,
                                                      double sampleRate)
{
    // RBJ "Audio EQ Cookbook": the bilinear transform, prewarped at the centre frequency.
    frequency = std::clamp(frequency, 1.0, sampleRate * 0.4999);
    q = std::max(q, 0.025);
    const double w0 = 2.0 * pi * frequency / sampleRate;
    const double a = std::pow(10.0, gainDb / 40.0);
    const double cw = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * q);
    const double twoSqrtAAlpha = 2.0 * std::sqrt(a) * alpha;
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = -2.0 * cw, a2 = 0.0;
    a0 = 1.0 + alpha;
    a2 = 1.0 - alpha;
    switch (type)
    {
    case BiquadType::LowPass:
        b0 = 0.5 * (1.0 - cw);
        b1 = 1.0 - cw;
        b2 = b0;
        break;
    case BiquadType::HighPass:
        b0 = 0.5 * (1.0 + cw);
        b1 = -(1.0 + cw);
        b2 = b0;
        break;
    case BiquadType::BandPass:
        b0 = alpha;
        b1 = 0.0;
        b2 = -alpha;
        break;
    case BiquadType::Notch:
        b0 = 1.0;
        b1 = -2.0 * cw;
        b2 = 1.0;
        break;
    case BiquadType::Peak:
        b0 = 1.0 + alpha * a;
        b1 = -2.0 * cw;
        b2 = 1.0 - alpha * a;
        a0 = 1.0 + alpha / a;
        a2 = 1.0 - alpha / a;
        break;
    case BiquadType::LowShelf:
        b0 = a * ((a + 1.0) - (a - 1.0) * cw + twoSqrtAAlpha);
        b1 = 2.0 * a * ((a - 1.0) - (a + 1.0) * cw);
        b2 = a * ((a + 1.0) - (a - 1.0) * cw - twoSqrtAAlpha);
        a0 = (a + 1.0) + (a - 1.0) * cw + twoSqrtAAlpha;
        a1 = -2.0 * ((a - 1.0) + (a + 1.0) * cw);
        a2 = (a + 1.0) + (a - 1.0) * cw - twoSqrtAAlpha;
        break;
    case BiquadType::HighShelf:
        b0 = a * ((a + 1.0) + (a - 1.0) * cw + twoSqrtAAlpha);
        b1 = -2.0 * a * ((a - 1.0) + (a + 1.0) * cw);
        b2 = a * ((a + 1.0) + (a - 1.0) * cw - twoSqrtAAlpha);
        a0 = (a + 1.0) - (a - 1.0) * cw + twoSqrtAAlpha;
        a1 = 2.0 * ((a - 1.0) - (a + 1.0) * cw);
        a2 = (a + 1.0) - (a - 1.0) * cw - twoSqrtAAlpha;
        break;
    }
    BiquadCoefficients c;
    c.b0 = b0 / a0;
    c.b1 = b1 / a0;
    c.b2 = b2 / a0;
    c.a1 = a1 / a0;
    c.a2 = a2 / a0;
    return c;
}

BiquadCoefficients BiquadCoefficients::design(BiquadType type, double frequency, double q, double gainDb,
                                              double sampleRate)
{
    if (type == BiquadType::Notch || type == BiquadType::LowShelf || type == BiquadType::HighShelf)
        return designBilinear(type, frequency, q, gainDb, sampleRate);

    frequency = std::clamp(frequency, 1.0, sampleRate * 0.4999);
    q = std::max(q, 0.025);
    const double w0 = 2.0 * pi * frequency / sampleRate;
    BiquadCoefficients c;

    // Matched designs (Vicanek). The poles' Q for a peak is A q, so cuts and boosts mirror.
    const double a = std::pow(10.0, gainDb / 40.0);
    const double poleQ = type == BiquadType::Peak ? q * a : q;
    const auto poles = matchedPoles(w0, poleQ);
    c.a1 = poles.a1;
    c.a2 = poles.a2;
    const double A0 = (1.0 + poles.a1 + poles.a2) * (1.0 + poles.a1 + poles.a2);
    const double A1 = (1.0 - poles.a1 + poles.a2) * (1.0 - poles.a1 + poles.a2);
    const double A2 = -4.0 * poles.a2;
    const PowerTerms at(w0);
    const double denominatorAtCentre = A0 * at.phi0 + A1 * at.phi1 + A2 * at.phi2;
    // The analog magnitude at Nyquist (the digital Nyquist, f0 = w0 here).
    const double nyquist = sampleRate * 0.5;
    const double analogAtNyquist = std::norm(analogResponse(type, nyquist, frequency, q, gainDb));
    const double analogAtCentre = std::norm(analogResponse(type, frequency, frequency, q, gainDb));
    const double R1 = denominatorAtCentre * analogAtCentre;

    switch (type)
    {
    case BiquadType::LowPass:
    {
        // Match DC, the centre and Nyquist (Vicanek's two-point version, with b2 = 0, drifts by a
        // few dB near Nyquist for high cutoffs).
        const double B0 = A0;
        const double B1 = A1 * analogAtNyquist;
        const double B2 = (R1 - B0 * at.phi0 - B1 * at.phi1) / at.phi2;
        const double W = 0.5 * (std::sqrt(B0) + std::sqrt(B1));
        if (W * W + B2 >= 0.0)
        {
            c.b0 = 0.5 * (W + std::sqrt(W * W + B2));
            c.b1 = 0.5 * (std::sqrt(B0) - std::sqrt(B1));
            c.b2 = -B2 / (4.0 * c.b0);
        }
        else
        {
            const double B1two = std::max(0.0, (R1 - B0 * at.phi0) / at.phi1);
            c.b0 = 0.5 * (std::sqrt(B0) + std::sqrt(B1two));
            c.b1 = std::sqrt(B0) - c.b0;
            c.b2 = 0.0;
        }
        break;
    }
    case BiquadType::HighPass:
    {
        // A double zero at DC; match the centre.
        c.b0 = std::sqrt(R1) / (4.0 * at.phi1);
        c.b1 = -2.0 * c.b0;
        c.b2 = c.b0;
        break;
    }
    case BiquadType::BandPass:
    {
        // A zero at DC; match the centre and Nyquist.
        const double B1 = A1 * analogAtNyquist;
        const double B2 = (R1 - B1 * at.phi1) / at.phi2;
        c.b1 = -0.5 * std::sqrt(B1);
        c.b0 = 0.5 * (std::sqrt(std::max(0.0, B2 + c.b1 * c.b1)) - c.b1);
        c.b2 = -c.b0 - c.b1;
        break;
    }
    case BiquadType::Peak:
    default:
    {
        // Match DC, the centre and Nyquist.
        const double B0 = A0;
        const double B1 = A1 * analogAtNyquist;
        const double B2 = (R1 - B0 * at.phi0 - B1 * at.phi1) / at.phi2;
        const double W = 0.5 * (std::sqrt(B0) + std::sqrt(B1));
        c.b0 = 0.5 * (W + std::sqrt(std::max(0.0, W * W + B2)));
        c.b1 = 0.5 * (std::sqrt(B0) - std::sqrt(B1));
        c.b2 = -B2 / (4.0 * c.b0);
        break;
    }
    }
    return c;
}

std::complex<double> BiquadCoefficients::response(double frequency, double sampleRate) const
{
    const double w = 2.0 * pi * frequency / sampleRate;
    const std::complex<double> z1 = std::polar(1.0, -w);
    const std::complex<double> z2 = z1 * z1;
    return (b0 + b1 * z1 + b2 * z2) / (1.0 + a1 * z1 + a2 * z2);
}

} // namespace lattice::dsp
