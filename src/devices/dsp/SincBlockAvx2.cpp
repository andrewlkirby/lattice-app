// Compiled with AVX2 + FMA (CMakeLists.txt sets the flags for *Avx2.cpp on x86-64). Include nothing
// but the plain interface and the intrinsics: see SincBlock.h.

#include "devices/dsp/SincBlock.h"

#if LATTICE_HAVE_AVX2_KERNELS
#include <immintrin.h>

namespace lattice::dsp
{

namespace
{
/** Sum of the 8 lanes. */
inline float horizontalSum(__m256 v) noexcept
{
    const __m128 lo = _mm256_castps256_ps128(v);
    const __m128 hi = _mm256_extractf128_ps(v, 1);
    __m128 s = _mm_add_ps(lo, hi);
    s = _mm_add_ps(s, _mm_movehl_ps(s, s));
    s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 0x55));
    return _mm_cvtss_f32(s);
}
} // namespace

void sincBlockAvx2(const SincKernelView& k, const float* s0, const float* s1, const std::int64_t* index,
                   const float* fraction, int n, float* outL, float* outR) noexcept
{
    const bool stereo = s1 != s0;
    const float phases = static_cast<float>(k.phases);
    for (int i = 0; i < n; ++i)
    {
        const float scaled = fraction[i] * phases;
        int phase = static_cast<int>(scaled);
        if (phase > k.phases - 1)
            phase = k.phases - 1;
        const __m256 w = _mm256_set1_ps(scaled - static_cast<float>(phase));
        const float* row0 = k.rows + static_cast<std::int64_t>(phase) * k.stride;
        const float* row1 = row0 + k.stride;
        const std::int64_t first = index[i] - k.taps / 2 + 1;
        const float* a = s0 + first;
        const float* b = s1 + first;
        // Two accumulators per channel hide the FMA latency.
        __m256 accA0 = _mm256_setzero_ps(), accA1 = _mm256_setzero_ps();
        __m256 accB0 = _mm256_setzero_ps(), accB1 = _mm256_setzero_ps();
        int j = 0;
        if (stereo)
        {
            for (; j + 16 <= k.stride; j += 16)
            {
                const __m256 r0a = _mm256_loadu_ps(row0 + j), r1a = _mm256_loadu_ps(row1 + j);
                const __m256 r0b = _mm256_loadu_ps(row0 + j + 8), r1b = _mm256_loadu_ps(row1 + j + 8);
                const __m256 ca = _mm256_fmadd_ps(w, _mm256_sub_ps(r1a, r0a), r0a);
                const __m256 cb = _mm256_fmadd_ps(w, _mm256_sub_ps(r1b, r0b), r0b);
                accA0 = _mm256_fmadd_ps(ca, _mm256_loadu_ps(a + j), accA0);
                accB0 = _mm256_fmadd_ps(ca, _mm256_loadu_ps(b + j), accB0);
                accA1 = _mm256_fmadd_ps(cb, _mm256_loadu_ps(a + j + 8), accA1);
                accB1 = _mm256_fmadd_ps(cb, _mm256_loadu_ps(b + j + 8), accB1);
            }
            for (; j < k.stride; j += 8)
            {
                const __m256 r0 = _mm256_loadu_ps(row0 + j);
                const __m256 c = _mm256_fmadd_ps(w, _mm256_sub_ps(_mm256_loadu_ps(row1 + j), r0), r0);
                accA0 = _mm256_fmadd_ps(c, _mm256_loadu_ps(a + j), accA0);
                accB0 = _mm256_fmadd_ps(c, _mm256_loadu_ps(b + j), accB0);
            }
            outL[i] = horizontalSum(_mm256_add_ps(accA0, accA1));
            outR[i] = horizontalSum(_mm256_add_ps(accB0, accB1));
        }
        else
        {
            for (; j + 16 <= k.stride; j += 16)
            {
                const __m256 r0a = _mm256_loadu_ps(row0 + j), r1a = _mm256_loadu_ps(row1 + j);
                const __m256 r0b = _mm256_loadu_ps(row0 + j + 8), r1b = _mm256_loadu_ps(row1 + j + 8);
                accA0 = _mm256_fmadd_ps(_mm256_fmadd_ps(w, _mm256_sub_ps(r1a, r0a), r0a), _mm256_loadu_ps(a + j), accA0);
                accA1 = _mm256_fmadd_ps(_mm256_fmadd_ps(w, _mm256_sub_ps(r1b, r0b), r0b), _mm256_loadu_ps(a + j + 8),
                                        accA1);
            }
            for (; j < k.stride; j += 8)
            {
                const __m256 r0 = _mm256_loadu_ps(row0 + j);
                accA0 = _mm256_fmadd_ps(_mm256_fmadd_ps(w, _mm256_sub_ps(_mm256_loadu_ps(row1 + j), r0), r0),
                                        _mm256_loadu_ps(a + j), accA0);
            }
            outL[i] = outR[i] = horizontalSum(_mm256_add_ps(accA0, accA1));
        }
    }
    _mm256_zeroupper();
}

} // namespace lattice::dsp

#endif // LATTICE_HAVE_AVX2_KERNELS
