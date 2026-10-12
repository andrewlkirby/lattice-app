#include "devices/dsp/SincBlock.h"

#include "devices/dsp/SincResampler.h"

#include <algorithm>

#include <juce_core/juce_core.h>

#if LATTICE_SINC_SSE
#include <xmmintrin.h>
#endif

namespace lattice::dsp
{

void sincBlockGeneric(const SincKernelView& k, const float* s0, const float* s1, const std::int64_t* index,
                      const float* fraction, int n, float* outL, float* outR) noexcept
{
    const float* data[2] = {s0, s1};
    for (int i = 0; i < n; ++i)
    {
        float out[2];
        sincInterpolate(k, data, s1 != s0 ? 2 : 1, index[i], fraction[i], out);
        outL[i] = out[0];
        outR[i] = out[1];
    }
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

SincBlockFunction sincBlock() noexcept
{
#if LATTICE_HAVE_AVX2_KERNELS
    static const SincBlockFunction best = juce::SystemStats::hasAVX2() && juce::SystemStats::hasFMA3()
                                              ? &sincBlockAvx2
                                              : &sincBlockGeneric;
    return best;
#else
    return &sincBlockGeneric;
#endif
}

} // namespace lattice::dsp
