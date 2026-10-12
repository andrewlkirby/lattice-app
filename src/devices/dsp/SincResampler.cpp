#include "devices/dsp/SincResampler.h"

#include <cmath>

namespace lattice::dsp
{
namespace
{
constexpr double pi = 3.14159265358979323846;

/** Modified Bessel function of the first kind, order 0 (series). */
double besselI0(double x)
{
    double sum = 1.0;
    double term = 1.0;
    const double q = x * x * 0.25;
    for (int k = 1; k < 64; ++k)
    {
        term *= q / (static_cast<double>(k) * k);
        sum += term;
        if (term < sum * 1e-17)
            break;
    }
    return sum;
}

double sinc(double x)
{
    return std::abs(x) < 1e-12 ? 1.0 : std::sin(pi * x) / (pi * x);
}
} // namespace

SincKernelBank::SincKernelBank()
{
    const double i0Beta = besselI0(beta);
    for (int k = 0; k < numLevels; ++k)
    {
        auto& level = levels[static_cast<std::size_t>(k)];
        level.bandwidth = std::exp2(-static_cast<double>(k) / levelsPerOctave);
        const double fc = cutoff * level.bandwidth;     // cycles per input sample
        const double halfWidth = zeroCrossings / level.bandwidth; // input samples each side
        level.taps = 2 * static_cast<int>(std::ceil(halfWidth));
        level.stride = (level.taps + 7) / 8 * 8; // whole AVX vectors
        // Stretched kernels change more slowly per input sample, so need fewer phase rows.
        level.phases = std::max(64, static_cast<int>(std::lround(512.0 * level.bandwidth)));
        level.rows.assign(static_cast<std::size_t>(level.phases + 1) * static_cast<std::size_t>(level.stride), 0.0f);

        for (int p = 0; p <= level.phases; ++p)
        {
            const double f = static_cast<double>(p) / level.phases;
            float* row = level.rows.data() + static_cast<std::size_t>(p) * static_cast<std::size_t>(level.stride);
            double sum = 0.0;
            std::vector<double> w(static_cast<std::size_t>(level.taps));
            for (int j = 0; j < level.taps; ++j)
            {
                const double t = static_cast<double>(j - level.taps / 2 + 1) - f; // offset from the position
                const double r = t / halfWidth;
                const double window = std::abs(r) < 1.0 ? besselI0(beta * std::sqrt(1.0 - r * r)) / i0Beta : 0.0;
                w[static_cast<std::size_t>(j)] = 2.0 * fc * sinc(2.0 * fc * t) * window;
                sum += w[static_cast<std::size_t>(j)];
            }
            for (int j = 0; j < level.taps; ++j)
                row[j] = static_cast<float>(w[static_cast<std::size_t>(j)] / sum); // exactly unity gain at DC
        }
    }
}

const SincKernelBank& SincKernelBank::get()
{
    static const SincKernelBank bank;
    (void)sincBlock(); // the CPU check, made here on the message thread rather than in playback
    return bank;
}

const SincKernelBank::Level& SincKernelBank::levelFor(double ratio) const noexcept
{
    if (ratio <= 1.0)
        return levels[0];
    // The level whose bandwidth is just above 1 / ratio: the pass band stays flat to 0.9 x Nyquist,
    // and what folds over (at most the top 1/8 octave) lands above 0.9 x Nyquist.
    const int k = static_cast<int>(std::floor(std::log2(ratio) * levelsPerOctave + 1e-9));
    return levels[static_cast<std::size_t>(std::min(k, numLevels - 1))];
}

} // namespace lattice::dsp
