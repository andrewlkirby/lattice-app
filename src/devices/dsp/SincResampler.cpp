#include "devices/dsp/SincResampler.h"

#include <cmath>
#include <cstdint>

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
        const auto stride = static_cast<std::size_t>(level.stride);
        constexpr std::size_t alignBytes = 64; // the rows never split a cache line
        level.storage.assign(static_cast<std::size_t>(level.phases) * 2 * stride + alignBytes / sizeof(float), 0.0f);
        const auto address = reinterpret_cast<std::uintptr_t>(level.storage.data());
        level.offset = (alignBytes - address % alignBytes) % alignBytes / sizeof(float);
        float* rows = level.storage.data() + level.offset;

        // The weights at phase p of `phases` (the last, p = phases, is the next sample's phase 0).
        const auto weightsAt = [&](int p, std::vector<double>& w) {
            const double f = static_cast<double>(p) / level.phases;
            double sum = 0.0;
            for (int j = 0; j < level.taps; ++j)
            {
                const double t = static_cast<double>(j - level.taps / 2 + 1) - f; // offset from the position
                const double r = t / halfWidth;
                const double window = std::abs(r) < 1.0 ? besselI0(beta * std::sqrt(1.0 - r * r)) / i0Beta : 0.0;
                w[static_cast<std::size_t>(j)] = 2.0 * fc * sinc(2.0 * fc * t) * window;
                sum += w[static_cast<std::size_t>(j)];
            }
            for (auto& x : w)
                x /= sum; // exactly unity gain at DC
        };
        std::vector<double> current(static_cast<std::size_t>(level.taps)), next(current.size());
        weightsAt(0, current);
        for (int p = 0; p < level.phases; ++p)
        {
            weightsAt(p + 1, next);
            float* row = rows + static_cast<std::size_t>(p) * 2 * stride;
            for (std::size_t j = 0; j < current.size(); ++j)
            {
                row[j] = static_cast<float>(current[j]);
                row[stride + j] = static_cast<float>(next[j] - current[j]);
            }
            std::swap(current, next);
        }
    }
}

const SincKernelBank& SincKernelBank::get()
{
    static const SincKernelBank bank;
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
