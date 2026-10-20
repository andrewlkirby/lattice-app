// Compiled with AVX2 + FMA (CMakeLists.txt sets the flags for *Avx2.cpp on x86-64). Include nothing
// but the plain interface and the intrinsics: see SincBlock.h.

#include "devices/dsp/SincBlock.h"

#if LATTICE_HAVE_AVX2_KERNELS
#include <cstddef>
#include <immintrin.h>

// The helpers must inline (MSVC otherwise calls them per step with the registers in memory), and
// the loops over the four positions must unroll (GCC doesn't at -O2) so the accumulators stay in
// registers.
#if defined(_MSC_VER) && !defined(__clang__)
#define LATTICE_SINC_INLINE __forceinline
#define LATTICE_SINC_UNROLL
#elif defined(__clang__)
#define LATTICE_SINC_INLINE inline __attribute__((always_inline))
#define LATTICE_SINC_UNROLL _Pragma("unroll")
#else
#define LATTICE_SINC_INLINE inline __attribute__((always_inline))
#define LATTICE_SINC_UNROLL _Pragma("GCC unroll 4")
#endif

namespace lattice::dsp
{

namespace
{
constexpr int batch = 4; // positions per step: their sums reduce together

/** The sums of each register's lanes, as four floats. */
LATTICE_SINC_INLINE __m128 laneSums(__m256 a0, __m256 a1, __m256 a2, __m256 a3) noexcept
{
    const __m256 h = _mm256_hadd_ps(_mm256_hadd_ps(a0, a1), _mm256_hadd_ps(a2, a3));
    return _mm_add_ps(_mm256_castps256_ps128(h), _mm256_extractf128_ps(h, 1));
}

/** Four positions at once (one kernel level, so one length): each reads its own phase row, and
    the sums reduce together. Positions past `count` repeat the first and are not stored. */
template <bool Stereo>
LATTICE_SINC_INLINE void fourPositions(const SincKernelView& k, const float* s0, const float* s1,
                                       const std::int64_t* index, const float* fraction, int count, float* outL,
                                       float* outR) noexcept
{
    const float phases = static_cast<float>(k.phases);
    const std::ptrdiff_t stride = k.stride;
    const std::ptrdiff_t toRight = s1 - s0; // the right channel, from the left
    const float* row[batch];
    const float* a[batch];
    __m256 w[batch];
    LATTICE_SINC_UNROLL
    for (int q = 0; q < batch; ++q)
    {
        const int from = q < count ? q : 0;
        const float scaled = fraction[from] * phases;
        int phase = static_cast<int>(scaled);
        if (phase > k.phases - 1)
            phase = k.phases - 1;
        w[q] = _mm256_set1_ps(scaled - static_cast<float>(phase));
        row[q] = k.rows + static_cast<std::ptrdiff_t>(phase) * 2 * stride;
        a[q] = s0 + (index[from] - k.taps / 2 + 1);
    }

    __m256 accL[batch], accR[batch];
    LATTICE_SINC_UNROLL
    for (int q = 0; q < batch; ++q)
        accL[q] = accR[q] = _mm256_setzero_ps();
    for (std::ptrdiff_t j = 0; j < stride; j += 8)
    {
        LATTICE_SINC_UNROLL
        for (int q = 0; q < batch; ++q)
        {
            // The weights between this phase and the next: row + w x difference.
            const __m256 c = _mm256_fmadd_ps(w[q], _mm256_load_ps(row[q] + j + stride), _mm256_load_ps(row[q] + j));
            accL[q] = _mm256_fmadd_ps(c, _mm256_loadu_ps(a[q] + j), accL[q]);
            if constexpr (Stereo)
                accR[q] = _mm256_fmadd_ps(c, _mm256_loadu_ps(a[q] + j + toRight), accR[q]);
        }
    }

    alignas(16) float left[batch], right[batch];
    _mm_store_ps(left, laneSums(accL[0], accL[1], accL[2], accL[3]));
    if constexpr (Stereo)
        _mm_store_ps(right, laneSums(accR[0], accR[1], accR[2], accR[3]));
    for (int q = 0; q < count; ++q)
    {
        outL[q] = left[q];
        outR[q] = Stereo ? right[q] : left[q];
    }
}

template <bool Stereo>
void run(const SincKernelView& k, const float* s0, const float* s1, const std::int64_t* index,
         const float* fraction, int n, float* outL, float* outR) noexcept
{
    for (int i = 0; i < n; i += batch)
        fourPositions<Stereo>(k, s0, s1, index + i, fraction + i, n - i < batch ? n - i : batch, outL + i, outR + i);
}
} // namespace

void sincBlockAvx2(const SincKernelView& k, const float* s0, const float* s1, const std::int64_t* index,
                   const float* fraction, int n, float* outL, float* outR) noexcept
{
    if (s1 != s0)
        run<true>(k, s0, s1, index, fraction, n, outL, outR);
    else
        run<false>(k, s0, s1, index, fraction, n, outL, outR);
    _mm256_zeroupper();
}

} // namespace lattice::dsp

#endif // LATTICE_HAVE_AVX2_KERNELS
