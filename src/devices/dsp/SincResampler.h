#pragma once

#include "core/RealtimeCheck.h"

#include <algorithm>
#include <array>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#define LATTICE_SINC_SSE 1
#include <xmmintrin.h>
#else
#define LATTICE_SINC_SSE 0
#endif

namespace lattice::dsp
{

/** Windowed-sinc interpolation kernels for playing samples at any pitch (docs/04-m3-devices.md,
    "Sampler": interpolation quality).

    Each kernel is a sinc with 32 zero crossings on each side under a Kaiser window (beta 10): flat
    within 0.001 dB up to 0.9 x Nyquist with more than 90 dB of stop band, measured. (The plan's
    first sketch said 32 taps; 32 taps can't reach -90 dB and 0.1 dB flatness together, so the kernel
    has 64 at unity.)

    Pitching up by a ratio r > 1 would alias with the unity kernel, so its bandwidth is scaled down
    to about 1/r: levels in 1/8-octave steps down to 4 octaves up, each stretched to keep 32 zero
    crossings a side. The level used is the one just above 1/r, so the pass band stays flat to
    0.9 x Nyquist and the little that folds over lands above 0.9 x Nyquist. Each kernel is stored
    polyphase (rows of taps for evenly spaced fractional positions) and rows are interpolated
    linearly. Built once, shared; reading is real-time safe. */
class SincKernelBank
{
public:
    static constexpr int zeroCrossings = 32;
    static constexpr int levelsPerOctave = 8;
    static constexpr int numLevels = 4 * levelsPerOctave + 1;
    static constexpr double beta = 10.0;
    static constexpr double cutoff = 0.4975; // cycles per sample at unity (Nyquist is 0.5)

    struct Level
    {
        double bandwidth = 1.0; // 1 at unity, 1/r when pitching up by r
        int taps = 0;           // even; samples from i - taps/2 + 1 to i + taps/2 around position i + f
        int stride = 0;         // taps rounded up to a multiple of 4 (zero weights at the end)
        int phases = 0;
        std::vector<float> rows; // (phases + 1) rows of `stride` weights
    };

    static const SincKernelBank& get();

    /** The level for playing `ratio` input samples per output sample. */
    const Level& levelFor(double ratio) const noexcept LATTICE_NONBLOCKING;
    const Level& level(int index) const noexcept { return levels[static_cast<std::size_t>(index)]; }
    /** The most taps any level has (with its padding): samples need half this much silence on
        each side. */
    int maxTaps() const noexcept { return levels.back().stride; }

private:
    SincKernelBank();
    std::array<Level, numLevels> levels;
};

/** Interpolates up to two channels at a fractional position with one kernel level. data[c] must
    be readable from index - taps/2 + 1 to index + taps/2. */
inline void sincInterpolate(const SincKernelBank::Level& k, const float* const* data, int numChannels,
                            long long index, float fraction, float* out) noexcept LATTICE_NONBLOCKING
{
    const float scaled = fraction * static_cast<float>(k.phases);
    const int phase = std::min(static_cast<int>(scaled), k.phases - 1);
    const float w = scaled - static_cast<float>(phase);
    const float* row0 = k.rows.data() + static_cast<std::size_t>(phase) * static_cast<std::size_t>(k.stride);
    const float* row1 = row0 + k.stride;
    const long long first = index - k.taps / 2 + 1;
    const float* s0 = data[0] + first;
    const float* s1 = numChannels > 1 ? data[1] + first : s0;
#if LATTICE_SINC_SSE
    const __m128 wv = _mm_set1_ps(w);
    __m128 acc0 = _mm_setzero_ps();
    __m128 acc1 = _mm_setzero_ps();
    if (numChannels > 1)
    {
        for (int j = 0; j < k.stride; j += 4)
        {
            const __m128 r0 = _mm_loadu_ps(row0 + j);
            const __m128 c = _mm_add_ps(r0, _mm_mul_ps(wv, _mm_sub_ps(_mm_loadu_ps(row1 + j), r0)));
            acc0 = _mm_add_ps(acc0, _mm_mul_ps(c, _mm_loadu_ps(s0 + j)));
            acc1 = _mm_add_ps(acc1, _mm_mul_ps(c, _mm_loadu_ps(s1 + j)));
        }
    }
    else
    {
        for (int j = 0; j < k.stride; j += 4)
        {
            const __m128 r0 = _mm_loadu_ps(row0 + j);
            const __m128 c = _mm_add_ps(r0, _mm_mul_ps(wv, _mm_sub_ps(_mm_loadu_ps(row1 + j), r0)));
            acc0 = _mm_add_ps(acc0, _mm_mul_ps(c, _mm_loadu_ps(s0 + j)));
        }
        acc1 = acc0;
    }
    // Horizontal sums of both accumulators at once.
    const __m128 t0 = _mm_add_ps(_mm_unpacklo_ps(acc0, acc1), _mm_unpackhi_ps(acc0, acc1)); // a01 b01 a23 b23
    const __m128 t1 = _mm_add_ps(t0, _mm_movehl_ps(t0, t0));
    alignas(16) float sums[4];
    _mm_store_ps(sums, t1);
    out[0] = sums[0];
    out[1] = sums[1];
#else
    float acc[2] = {0.0f, 0.0f};
    for (int j = 0; j < k.stride; ++j)
    {
        const float c = row0[j] + w * (row1[j] - row0[j]);
        acc[0] += c * s0[j];
        acc[1] += c * s1[j];
    }
    out[0] = acc[0];
    out[1] = acc[1];
#endif
}

/** Cubic Hermite (Catmull-Rom) interpolation: the "Draft" quality. data readable from index - 1
    to index + 2. */
inline void hermiteInterpolate(const float* const* data, int numChannels, long long index, float t,
                               float* out) noexcept LATTICE_NONBLOCKING
{
    for (int c = 0; c < 2; ++c)
    {
        const float* p = data[c < numChannels ? c : 0] + index - 1;
        const float c1 = 0.5f * (p[2] - p[0]);
        const float c2 = p[0] - 2.5f * p[1] + 2.0f * p[2] - 0.5f * p[3];
        const float c3 = 0.5f * (p[3] - p[0]) + 1.5f * (p[1] - p[2]);
        out[c] = ((c3 * t + c2) * t + c1) * t + p[1];
    }
}

} // namespace lattice::dsp
