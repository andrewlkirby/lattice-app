// Compiled with AVX2 + FMA (CMakeLists.txt sets the flags for *Avx2.cpp on x86-64). Include nothing
// but the plain interface, the intrinsics and the shared kernels: see VoiceFilterKernels.h.

#include "devices/dsp/VoiceFilterKernels.h"

#if LATTICE_HAVE_AVX2_VOICE_FILTERS
#include <immintrin.h>

#include "devices/dsp/VoiceFilterKernels.inl"

namespace lattice::dsp
{
namespace
{
/** Eight lanes per AVX register, with FMA (VoiceFilterKernels.inl). */
struct Avx
{
    using reg = __m256;
    using mask = __m256;
    static constexpr int width = 8;
    static constexpr int chunk = 8;

    static LATTICE_KERNEL_INLINE reg set1(float v) noexcept { return _mm256_set1_ps(v); }
    static LATTICE_KERNEL_INLINE reg load(const float* p) noexcept { return _mm256_load_ps(p); }
    static LATTICE_KERNEL_INLINE void store(float* p, reg v) noexcept { _mm256_store_ps(p, v); }
    static LATTICE_KERNEL_INLINE reg add(reg a, reg b) noexcept { return _mm256_add_ps(a, b); }
    static LATTICE_KERNEL_INLINE reg sub(reg a, reg b) noexcept { return _mm256_sub_ps(a, b); }
    static LATTICE_KERNEL_INLINE reg mul(reg a, reg b) noexcept { return _mm256_mul_ps(a, b); }
    static LATTICE_KERNEL_INLINE reg div(reg a, reg b) noexcept { return _mm256_div_ps(a, b); }
    static LATTICE_KERNEL_INLINE reg madd(reg a, reg b, reg c) noexcept { return _mm256_fmadd_ps(a, b, c); }
    static LATTICE_KERNEL_INLINE reg nmadd(reg a, reg b, reg c) noexcept { return _mm256_fnmadd_ps(a, b, c); }
    static LATTICE_KERNEL_INLINE reg max(reg a, reg b) noexcept { return _mm256_max_ps(a, b); }
    static LATTICE_KERNEL_INLINE reg abs(reg a) noexcept
    {
        return _mm256_and_ps(a, _mm256_castsi256_ps(_mm256_set1_epi32(0x7fffffff)));
    }
    static LATTICE_KERNEL_INLINE reg reciprocal(reg d) noexcept
    {
        const reg r = _mm256_rcp_ps(d); // 12 bits; one Newton-Raphson step makes about 23
        return _mm256_mul_ps(r, _mm256_fnmadd_ps(d, r, _mm256_set1_ps(2.0f)));
    }
    static LATTICE_KERNEL_INLINE mask lessEqual(reg a, reg b) noexcept { return _mm256_cmp_ps(a, b, _CMP_LE_OQ); }
    static LATTICE_KERNEL_INLINE reg select(mask m, reg a, reg b) noexcept { return _mm256_blendv_ps(b, a, m); }
    static LATTICE_KERNEL_INLINE reg gather(float* const* b, int i) noexcept
    {
        return _mm256_set_ps(b[7][i], b[6][i], b[5][i], b[4][i], b[3][i], b[2][i], b[1][i], b[0][i]);
    }
    static LATTICE_KERNEL_INLINE void scatter(float* const* b, int i, reg v) noexcept
    {
        alignas(32) float t[8];
        _mm256_store_ps(t, v);
        for (int l = 0; l < 8; ++l)
            b[l][i] = t[l];
    }
    /** 8 x 8 transpose: r[l] holds lane l's 8 samples before, sample l of the 8 lanes after. */
    static LATTICE_KERNEL_INLINE void transpose(reg* r) noexcept
    {
        const reg t0 = _mm256_unpacklo_ps(r[0], r[1]), t1 = _mm256_unpackhi_ps(r[0], r[1]);
        const reg t2 = _mm256_unpacklo_ps(r[2], r[3]), t3 = _mm256_unpackhi_ps(r[2], r[3]);
        const reg t4 = _mm256_unpacklo_ps(r[4], r[5]), t5 = _mm256_unpackhi_ps(r[4], r[5]);
        const reg t6 = _mm256_unpacklo_ps(r[6], r[7]), t7 = _mm256_unpackhi_ps(r[6], r[7]);
        const reg s0 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0));
        const reg s1 = _mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2));
        const reg s2 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0));
        const reg s3 = _mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(3, 2, 3, 2));
        const reg s4 = _mm256_shuffle_ps(t4, t6, _MM_SHUFFLE(1, 0, 1, 0));
        const reg s5 = _mm256_shuffle_ps(t4, t6, _MM_SHUFFLE(3, 2, 3, 2));
        const reg s6 = _mm256_shuffle_ps(t5, t7, _MM_SHUFFLE(1, 0, 1, 0));
        const reg s7 = _mm256_shuffle_ps(t5, t7, _MM_SHUFFLE(3, 2, 3, 2));
        r[0] = _mm256_permute2f128_ps(s0, s4, 0x20);
        r[1] = _mm256_permute2f128_ps(s1, s5, 0x20);
        r[2] = _mm256_permute2f128_ps(s2, s6, 0x20);
        r[3] = _mm256_permute2f128_ps(s3, s7, 0x20);
        r[4] = _mm256_permute2f128_ps(s0, s4, 0x31);
        r[5] = _mm256_permute2f128_ps(s1, s5, 0x31);
        r[6] = _mm256_permute2f128_ps(s2, s6, 0x31);
        r[7] = _mm256_permute2f128_ps(s3, s7, 0x31);
    }
    static LATTICE_KERNEL_INLINE void loadBlock(float* const* b, int i, reg* out) noexcept
    {
        for (int l = 0; l < 8; ++l)
            out[l] = _mm256_loadu_ps(b[l] + i);
        transpose(out);
    }
    static LATTICE_KERNEL_INLINE void storeBlock(float* const* b, int i, const reg* in) noexcept
    {
        reg r[8];
        for (int l = 0; l < 8; ++l)
            r[l] = in[l];
        transpose(r);
        for (int l = 0; l < 8; ++l)
            _mm256_storeu_ps(b[l] + i, r[l]);
    }
};
} // namespace

VoiceFilterKernels voiceFilterKernelsAvx2() noexcept
{
    return kernelsFor<Avx>();
}

} // namespace lattice::dsp
#endif
