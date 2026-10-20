#include "devices/dsp/SincBlock.h"

#include "devices/dsp/Simd.h"
#include "devices/dsp/SincResampler.h"

#include <algorithm>
#include <cstddef>

#include <juce_core/juce_core.h>

#if LATTICE_SINC_SSE
#include <xmmintrin.h>
#endif

namespace lattice::dsp
{

namespace
{
#if LATTICE_SINC_SSE
/** Four positions at once, as in SincBlockAvx2.cpp: each reads its own phase row, and the sums
    reduce together with one transpose. Positions past `count` repeat the first and are not
    stored. */
template <bool Stereo>
void fourPositions(const SincKernelView& k, const float* s0, const float* s1, const std::int64_t* index,
                   const float* fraction, int count, float* outL, float* outR) noexcept
{
    const float phases = static_cast<float>(k.phases);
    const std::ptrdiff_t stride = k.stride;
    const std::ptrdiff_t toRight = s1 - s0;
    const float* row[4];
    const float* a[4];
    __m128 w[4];
    for (int q = 0; q < 4; ++q)
    {
        const int from = q < count ? q : 0;
        const float scaled = fraction[from] * phases;
        const int phase = std::min(static_cast<int>(scaled), k.phases - 1);
        w[q] = _mm_set1_ps(scaled - static_cast<float>(phase));
        row[q] = k.rows + static_cast<std::ptrdiff_t>(phase) * 2 * stride;
        a[q] = s0 + (index[from] - k.taps / 2 + 1);
    }
    __m128 accL[4], accR[4];
    for (int q = 0; q < 4; ++q)
        accL[q] = accR[q] = _mm_setzero_ps();
    for (std::ptrdiff_t j = 0; j < stride; j += 4)
        for (int q = 0; q < 4; ++q)
        {
            const __m128 c = _mm_add_ps(_mm_load_ps(row[q] + j), _mm_mul_ps(w[q], _mm_load_ps(row[q] + j + stride)));
            accL[q] = _mm_add_ps(accL[q], _mm_mul_ps(c, _mm_loadu_ps(a[q] + j)));
            if constexpr (Stereo)
                accR[q] = _mm_add_ps(accR[q], _mm_mul_ps(c, _mm_loadu_ps(a[q] + j + toRight)));
        }
    const auto sums = [](__m128* acc) {
        _MM_TRANSPOSE4_PS(acc[0], acc[1], acc[2], acc[3]);
        return _mm_add_ps(_mm_add_ps(acc[0], acc[1]), _mm_add_ps(acc[2], acc[3]));
    };
    alignas(16) float left[4], right[4];
    _mm_store_ps(left, sums(accL));
    if constexpr (Stereo)
        _mm_store_ps(right, sums(accR));
    for (int q = 0; q < count; ++q)
    {
        outL[q] = left[q];
        outR[q] = Stereo ? right[q] : left[q];
    }
}

template <bool Stereo>
void runSse(const SincKernelView& k, const float* s0, const float* s1, const std::int64_t* index,
            const float* fraction, int n, float* outL, float* outR) noexcept
{
    for (int i = 0; i < n; i += 4)
        fourPositions<Stereo>(k, s0, s1, index + i, fraction + i, std::min(n - i, 4), outL + i, outR + i);
}
#endif
} // namespace

void sincBlockGeneric(const SincKernelView& k, const float* s0, const float* s1, const std::int64_t* index,
                      const float* fraction, int n, float* outL, float* outR) noexcept
{
#if LATTICE_SINC_SSE
    if (s1 != s0)
        runSse<true>(k, s0, s1, index, fraction, n, outL, outR);
    else
        runSse<false>(k, s0, s1, index, fraction, n, outL, outR);
#else
    const float* data[2] = {s0, s1};
    for (int i = 0; i < n; ++i)
    {
        float out[2];
        sincInterpolate(k, data, s1 != s0 ? 2 : 1, index[i], fraction[i], out);
        outL[i] = out[0];
        outR[i] = out[1];
    }
#endif
}

namespace
{
void hermiteChannel(const float* s, const std::int64_t* index, const float* fraction, int n, float* out) noexcept
{
    int i = 0;
#if LATTICE_SINC_SSE
    const __m128 half = _mm_set1_ps(0.5f), oneHalf = _mm_set1_ps(1.5f), two = _mm_set1_ps(2.0f),
                 twoHalf = _mm_set1_ps(2.5f);
    for (; i + 4 <= n; i += 4)
    {
        // The four points around each of four positions, transposed into p0..p3 across positions.
        __m128 p0 = _mm_loadu_ps(s + index[i] - 1);
        __m128 p1 = _mm_loadu_ps(s + index[i + 1] - 1);
        __m128 p2 = _mm_loadu_ps(s + index[i + 2] - 1);
        __m128 p3 = _mm_loadu_ps(s + index[i + 3] - 1);
        _MM_TRANSPOSE4_PS(p0, p1, p2, p3);
        const __m128 t = _mm_loadu_ps(fraction + i);
        const __m128 c1 = _mm_mul_ps(half, _mm_sub_ps(p2, p0));
        const __m128 c2 = _mm_sub_ps(_mm_add_ps(_mm_sub_ps(p0, _mm_mul_ps(twoHalf, p1)), _mm_mul_ps(two, p2)),
                                     _mm_mul_ps(half, p3));
        const __m128 c3 = _mm_add_ps(_mm_mul_ps(half, _mm_sub_ps(p3, p0)), _mm_mul_ps(oneHalf, _mm_sub_ps(p1, p2)));
        const __m128 y = _mm_add_ps(
            _mm_mul_ps(_mm_add_ps(_mm_mul_ps(_mm_add_ps(_mm_mul_ps(c3, t), c2), t), c1), t), p1);
        _mm_storeu_ps(out + i, y);
    }
#endif
    for (; i < n; ++i)
    {
        const float* p = s + index[i] - 1;
        const float t = fraction[i];
        const float c1 = 0.5f * (p[2] - p[0]);
        const float c2 = p[0] - 2.5f * p[1] + 2.0f * p[2] - 0.5f * p[3];
        const float c3 = 0.5f * (p[3] - p[0]) + 1.5f * (p[1] - p[2]);
        out[i] = ((c3 * t + c2) * t + c1) * t + p[1];
    }
}
} // namespace

void hermiteBlock(const float* s0, const float* s1, const std::int64_t* index, const float* fraction, int n,
                  float* outL, float* outR) noexcept
{
    hermiteChannel(s0, index, fraction, n, outL);
    if (s1 != s0)
        hermiteChannel(s1, index, fraction, n, outR);
    else
        std::copy_n(outL, n, outR);
}

SincBlockFunction sincBlockFor(SimdLevel level) noexcept
{
#if LATTICE_HAVE_AVX2_KERNELS
    if (level == SimdLevel::Avx2)
        return &sincBlockAvx2;
#endif
    juce::ignoreUnused(level);
    return &sincBlockGeneric;
}

SincBlockFunction sincBlock() noexcept
{
    return sincBlockFor(simdLevel());
}

} // namespace lattice::dsp
