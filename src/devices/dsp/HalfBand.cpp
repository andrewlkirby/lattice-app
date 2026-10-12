#include "devices/dsp/HalfBand.h"

#include <algorithm>
#include <cmath>

namespace lattice::dsp
{
namespace
{
constexpr double pi = 3.14159265358979323846;

double ipow(double x, int n)
{
    double r = 1.0;
    for (int i = 0; i < n; ++i)
        r *= x;
    return r;
}

double accNum(double q, int order, int c)
{
    double acc = 0.0;
    double term = 0.0;
    int i = 0;
    int sign = 1;
    do
    {
        term = ipow(q, i * (i + 1)) * std::sin((i * 2 + 1) * c * pi / order) * sign;
        acc += term;
        sign = -sign;
        ++i;
    } while (std::abs(term) > 1e-100 && i < 100);
    return acc;
}

double accDen(double q, int order, int c)
{
    double acc = 0.0;
    double term = 0.0;
    int i = 1;
    int sign = -1;
    do
    {
        term = ipow(q, i * i) * std::cos(i * 2 * c * pi / order) * sign;
        acc += term;
        sign = -sign;
        ++i;
    } while (std::abs(term) > 1e-100 && i < 100);
    return acc;
}
} // namespace

HalfBandDesign HalfBandDesign::make(double attenuationDb, double transition)
{
    // Transition parameter (elliptic modulus) and nome.
    double k = std::tan((1.0 - transition * 2.0) * pi / 4.0);
    k *= k;
    const double kksqrt = std::pow(1.0 - k * k, 0.25);
    const double e = 0.5 * (1.0 - kksqrt) / (1.0 + kksqrt);
    const double e2 = e * e;
    const double e4 = e2 * e2;
    const double q = e * (1.0 + e4 * (2.0 + e4 * (15.0 + 150.0 * e4)));

    // Filter order for the attenuation (always odd).
    const double attn = std::pow(10.0, -attenuationDb / 10.0);
    const double a = attn / (1.0 - attn);
    int order = static_cast<int>(std::ceil(std::log(a * a / 16.0) / std::log(q)));
    if (order % 2 == 0)
        ++order;
    order = std::max(order, 3);
    const int numCoefs = std::min((order - 1) / 2, HalfBand2x::maxCoefficients);

    HalfBandDesign d;
    for (int i = 0; i < numCoefs; ++i)
    {
        const int c = i + 1;
        const double num = accNum(q, order, c) * std::pow(q, 0.25);
        const double den = accDen(q, order, c) + 0.5;
        const double ww = num / den;
        const double wwsq = ww * ww;
        const double x = std::sqrt((1.0 - wwsq * k) * (1.0 - wwsq / k)) / (1.0 + wwsq);
        d.coefficients.push_back((1.0 - x) / (1.0 + x));
    }
    return d;
}

HalfBand2x::HalfBand2x(const HalfBandDesign& design)
{
    count = std::min(static_cast<int>(design.coefficients.size()), maxCoefficients);
    for (int i = 0; i < count; ++i)
    {
        auto& chainUp = (i % 2 == 0) ? upA : upB;
        auto& chainDown = (i % 2 == 0) ? downA : downB;
        const auto c = static_cast<float>(design.coefficients[static_cast<std::size_t>(i)]);
        chainUp.c[static_cast<std::size_t>(chainUp.n++)] = c;
        chainDown.c[static_cast<std::size_t>(chainDown.n++)] = c;
    }
}

HalfBand2x::HalfBand2x() : HalfBand2x([] {
                               static const HalfBandDesign voiceDesign = HalfBandDesign::make(80.0, 0.08);
                               return voiceDesign;
                           }())
{
}

void HalfBand2x::reset() noexcept
{
    upA.reset();
    upB.reset();
    downA.reset();
    downB.reset();
    downDelay = 0.0f;
}

void HalfBand2x::upsample(float in, float& out0, float& out1) noexcept
{
    out0 = upA.process(in);
    out1 = upB.process(in);
}

float HalfBand2x::downsample(float in0, float in1) noexcept
{
    const float a = downA.process(in0);
    const float b = downB.process(downDelay);
    downDelay = in1;
    return 0.5f * (a + b);
}

} // namespace lattice::dsp
