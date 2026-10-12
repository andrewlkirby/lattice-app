#include "devices/dsp/FirHalfBand.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#include <xmmintrin.h>
#define LATTICE_FIR_SSE 1
#else
#define LATTICE_FIR_SSE 0
#endif

namespace lattice::dsp
{
namespace
{
/** sum(taps[j] * window[j]) over n (a multiple of 4) values. */
inline float dot(const float* taps, const float* window, int n) noexcept
{
#if LATTICE_FIR_SSE
    __m128 acc = _mm_setzero_ps();
    for (int j = 0; j < n; j += 4)
        acc = _mm_add_ps(acc, _mm_mul_ps(_mm_loadu_ps(taps + j), _mm_loadu_ps(window + j)));
    acc = _mm_add_ps(acc, _mm_movehl_ps(acc, acc));
    acc = _mm_add_ss(acc, _mm_shuffle_ps(acc, acc, 1));
    return _mm_cvtss_f32(acc);
#else
    float acc = 0.0f;
    for (int j = 0; j < n; ++j)
        acc += taps[j] * window[j];
    return acc;
#endif
}

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
} // namespace

FirHalfBand::FirHalfBand(int taps, double attenuationDb)
{
    // 4k + 3 taps: an odd centre, with the half-band zeros on the other odd positions.
    numTaps = std::max(7, ((taps - 3 + 3) / 4) * 4 + 3);
    centre = (numTaps - 1) / 2;
    const double beta = attenuationDb > 50.0 ? 0.1102 * (attenuationDb - 8.7)
                                             : 0.5842 * std::pow(attenuationDb - 21.0, 0.4) + 0.07886 * (attenuationDb - 21.0);
    const double i0 = besselI0(beta);
    h.assign(static_cast<std::size_t>(numTaps), 0.0);
    double sum = 0.0;
    for (int n = 0; n < numTaps; ++n)
    {
        const int t = n - centre;
        double v = 0.0;
        if (t == 0)
            v = 0.5;
        else if (t % 2 != 0)
            v = std::sin(3.14159265358979323846 * t * 0.5) / (3.14159265358979323846 * t); // 2 fc sinc, fc = 1/4
        const double r = static_cast<double>(t) / centre;
        v *= besselI0(beta * std::sqrt(std::max(0.0, 1.0 - r * r))) / i0;
        h[static_cast<std::size_t>(n)] = v;
        sum += v;
    }
    // Unity gain at DC, and exactly 1/2 at the centre (the odd taps then sum to 1/2 too).
    double oddSum = sum - 0.5;
    for (int n = 0; n < numTaps; ++n)
        if (n != centre && h[static_cast<std::size_t>(n)] != 0.0)
            h[static_cast<std::size_t>(n)] *= 0.5 / oddSum;

    for (int n = 0; n < numTaps; n += 2)
        evenTaps.push_back(static_cast<float>(h[static_cast<std::size_t>(n)]));
    const int padded = (static_cast<int>(evenTaps.size()) + 3) / 4 * 4;
    reversed.assign(static_cast<std::size_t>(padded), 0.0f);
    for (std::size_t j = 0; j < evenTaps.size(); ++j)
        reversed[static_cast<std::size_t>(padded) - 1 - j] = evenTaps[j];
    length = padded + (centre + 1) / 2 + 4;
    upHistory.assign(static_cast<std::size_t>(2 * length), 0.0f);
    downEven.assign(static_cast<std::size_t>(2 * length), 0.0f);
    downOdd.assign(static_cast<std::size_t>(2 * length), 0.0f);
}

void FirHalfBand::reset() noexcept
{
    std::fill(upHistory.begin(), upHistory.end(), 0.0f);
    std::fill(downEven.begin(), downEven.end(), 0.0f);
    std::fill(downOdd.begin(), downOdd.end(), 0.0f);
    upPos = downPos = 0;
    std::fill(blockUp.begin(), blockUp.end(), 0.0f);
    std::fill(blockEven.begin(), blockEven.end(), 0.0f);
    std::fill(blockOdd.begin(), blockOdd.end(), 0.0f);
}

void FirHalfBand::prepareBlocks(int maxInput)
{
    // The even branch reaches back evenTaps - 1 inputs; the odd ones less.
    historyLength = static_cast<int>(evenTaps.size()) - 1;
    const auto size = static_cast<std::size_t>(historyLength + std::max(1, maxInput) + 4);
    blockUp.assign(size, 0.0f);
    blockEven.assign(size, 0.0f);
    blockOdd.assign(size, 0.0f);
}

namespace
{
/** y[m] = sum over j of taps[j] * x[m - j], for m in [0, n): x[-j] reaches into the history before
    x[0]. Four outputs at a time. */
void convolve(const float* taps, int numTaps, const float* x, int n, float* y) noexcept
{
    int m = 0;
#if LATTICE_FIR_SSE
    for (; m + 4 <= n; m += 4)
    {
        __m128 acc = _mm_setzero_ps();
        for (int j = 0; j < numTaps; ++j)
            acc = _mm_add_ps(acc, _mm_mul_ps(_mm_set1_ps(taps[j]), _mm_loadu_ps(x + m - j)));
        _mm_storeu_ps(y + m, acc);
    }
#endif
    for (; m < n; ++m)
    {
        float acc = 0.0f;
        for (int j = 0; j < numTaps; ++j)
            acc += taps[j] * x[m - j];
        y[m] = acc;
    }
}

/** Keeps the last `history` samples of a linear buffer for the next block. */
void keepHistory(std::vector<float>& buffer, int history, int n) noexcept
{
    std::memmove(buffer.data(), buffer.data() + n, static_cast<std::size_t>(history) * sizeof(float));
}
} // namespace

void FirHalfBand::upsampleBlock(const float* in, int n, float* out) noexcept
{
    const int taps = static_cast<int>(evenTaps.size());
    const int oddDelay = (centre - 1) / 2; // the centre tap (1/2, doubled) is a pure delay
    float* x = blockUp.data() + historyLength;
    std::copy_n(in, n, x);
    // Even outputs into the second half of `out` first, then interleave in place from the front.
    float* even = out + n;
    convolve(evenTaps.data(), taps, x, n, even);
    for (int m = 0; m < n; ++m)
    {
        const float e = 2.0f * even[m];
        out[2 * m + 1] = x[m - oddDelay];
        out[2 * m] = e; // out[2m] lies at or before even[m]'s slot, which is already read
    }
    keepHistory(blockUp, historyLength, n);
}

void FirHalfBand::downsampleBlock(const float* in, int n, float* out) noexcept
{
    const int taps = static_cast<int>(evenTaps.size());
    const int oddDelay = (centre + 1) / 2;
    float* even = blockEven.data() + historyLength;
    float* odd = blockOdd.data() + historyLength;
    for (int m = 0; m < n; ++m)
    {
        even[m] = in[2 * m];
        odd[m] = in[2 * m + 1];
    }
    convolve(evenTaps.data(), taps, even, n, out);
    for (int m = 0; m < n; ++m)
        out[m] += 0.5f * odd[m - oddDelay];
    keepHistory(blockEven, historyLength, n);
    keepHistory(blockOdd, historyLength, n);
}

void FirHalfBand::upsample(float in, float& out0, float& out1) noexcept
{
    // Written twice, so the window ending at upPos + length is always contiguous.
    if (++upPos == length) // (no %: a division per sample costs more than the filter)
        upPos = 0;
    upHistory[static_cast<std::size_t>(upPos)] = in;
    upHistory[static_cast<std::size_t>(upPos + length)] = in;
    const float* newest = upHistory.data() + upPos + length; // newest[-j] = x[m - j]
    const int padded = static_cast<int>(reversed.size());
    out0 = 2.0f * dot(reversed.data(), newest - (padded - 1), padded);
    out1 = newest[-(centre - 1) / 2];
}

float FirHalfBand::downsample(float in0, float in1) noexcept
{
    if (++downPos == length)
        downPos = 0;
    downEven[static_cast<std::size_t>(downPos)] = in0;
    downEven[static_cast<std::size_t>(downPos + length)] = in0;
    downOdd[static_cast<std::size_t>(downPos)] = in1;
    downOdd[static_cast<std::size_t>(downPos + length)] = in1;
    const float* even = downEven.data() + downPos + length;
    const float* odd = downOdd.data() + downPos + length;
    const int padded = static_cast<int>(reversed.size());
    return dot(reversed.data(), even - (padded - 1), padded) + 0.5f * odd[-(centre + 1) / 2];
}

} // namespace lattice::dsp
