// The oscillator kernels for any x86-64 CPU (SSE2), or plain C++ elsewhere (docs/04-m3-devices.md,
// "Oscillators": the inner loops are SIMD). The AVX2 versions are in OscillatorSimdAvx2.cpp.

#include "devices/dsp/OscillatorKernels.h"

#include "devices/dsp/Simd.h"

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#define LATTICE_OSC_SSE2 1
#include <emmintrin.h>
#include <xmmintrin.h>
#else
#define LATTICE_OSC_SSE2 0
#endif

namespace lattice::dsp
{
namespace
{
/** One Catmull-Rom read, as MipmappedWave::read (table points at x[-1]). */
inline float readScalar(const float* table, int bits, std::uint32_t phase) noexcept
{
    const float* p = table + (phase >> (32 - bits));
    const float frac = static_cast<float>(static_cast<std::uint32_t>(phase << bits)) * 2.3283064365386963e-10f;
    const float c1 = 0.5f * (p[2] - p[0]);
    const float c2 = p[0] - 2.5f * p[1] + 2.0f * p[2] - 0.5f * p[3];
    const float c3 = 0.5f * (p[3] - p[0]) + 1.5f * (p[1] - p[2]);
    return ((c3 * frac + c2) * frac + c1) * frac + p[1];
}

inline float valueScalar(const OscCopyJob& j, std::uint32_t phase) noexcept
{
    float x = readScalar(j.table0, j.bits0, phase);
    if (j.blend > 0.0f)
        x += j.blend * (readScalar(j.table1, j.bits1, phase) - x);
    return x;
}

/** The plain loop: one copy, one sample at a time. */
void copyScalar(OscCopyJob& j, int n, float* outL, float* outR) noexcept
{
    std::uint32_t p = j.phase;
    auto v = static_cast<std::int64_t>(j.inc);
    for (int i = 0; i < n; ++i)
    {
        v += j.step;
        float x = valueScalar(j, p);
        if (j.pulseOffset != 0)
            x -= valueScalar(j, p + j.pulseOffset);
        p += static_cast<std::uint32_t>(v);
        outL[i] += x * j.gainL;
        outR[i] += x * j.gainR;
    }
    j.phase = p;
    j.inc = static_cast<std::uint32_t>(v);
}

#if LATTICE_OSC_SSE2
/** Catmull-Rom reads of four phases from one table (table points at x[-1]). */
inline __m128 catmullRom(__m128i ph, const float* table, __m128i shift, __m128i fracShift) noexcept
{
    const __m128i index = _mm_srl_epi32(ph, shift);
    // The fraction: the bits below the index, as a 31-bit value (the conversion is signed).
    const __m128 frac = _mm_mul_ps(_mm_cvtepi32_ps(_mm_srli_epi32(_mm_sll_epi32(ph, fracShift), 1)),
                                   _mm_set1_ps(4.6566128730773926e-10f));
    alignas(16) std::int32_t idx[4];
    _mm_store_si128(reinterpret_cast<__m128i*>(idx), index);
    __m128 a = _mm_loadu_ps(table + idx[0]);
    __m128 b = _mm_loadu_ps(table + idx[1]);
    __m128 c = _mm_loadu_ps(table + idx[2]);
    __m128 d = _mm_loadu_ps(table + idx[3]);
    _MM_TRANSPOSE4_PS(a, b, c, d); // a = x[-1], b = x[0], c = x[1], d = x[2] in each lane
    const __m128 half = _mm_set1_ps(0.5f);
    const __m128 c1 = _mm_mul_ps(half, _mm_sub_ps(c, a));
    const __m128 c2 = _mm_sub_ps(_mm_add_ps(_mm_sub_ps(a, _mm_mul_ps(_mm_set1_ps(2.5f), b)), _mm_add_ps(c, c)),
                                 _mm_mul_ps(half, d));
    const __m128 c3 = _mm_add_ps(_mm_mul_ps(half, _mm_sub_ps(d, a)), _mm_mul_ps(_mm_set1_ps(1.5f), _mm_sub_ps(b, c)));
    return _mm_add_ps(_mm_mul_ps(_mm_add_ps(_mm_mul_ps(_mm_add_ps(_mm_mul_ps(c3, frac), c2), frac), c1), frac), b);
}

/** One copy, four samples at a time. */
void copySse(OscCopyJob& j, int n, float* outL, float* outR) noexcept
{
    const bool blending = j.blend > 0.0f;
    const __m128i shift0 = _mm_cvtsi32_si128(32 - j.bits0);
    const __m128i shift1 = _mm_cvtsi32_si128(32 - j.bits1);
    const __m128i fracShift0 = _mm_cvtsi32_si128(j.bits0);
    const __m128i fracShift1 = _mm_cvtsi32_si128(j.bits1);
    const __m128 blendV = _mm_set1_ps(j.blend);
    const __m128 gl = _mm_set1_ps(j.gainL);
    const __m128 gr = _mm_set1_ps(j.gainR);
    const __m128i offset = _mm_set1_epi32(static_cast<int>(j.pulseOffset));
    const auto value = [&](__m128i ph) {
        __m128 x = catmullRom(ph, j.table0, shift0, fracShift0);
        if (blending)
            x = _mm_add_ps(x, _mm_mul_ps(blendV, _mm_sub_ps(catmullRom(ph, j.table1, shift1, fracShift1), x)));
        return x;
    };

    std::uint32_t p = j.phase;
    auto v = static_cast<std::int64_t>(j.inc);
    int i = 0;
    for (; i + 4 <= n; i += 4)
    {
        alignas(16) std::uint32_t phases[4];
        for (int k = 0; k < 4; ++k)
        {
            v += j.step;
            phases[k] = p;
            p += static_cast<std::uint32_t>(v);
        }
        const __m128i ph = _mm_load_si128(reinterpret_cast<const __m128i*>(phases));
        __m128 x = value(ph);
        if (j.pulseOffset != 0)
            x = _mm_sub_ps(x, value(_mm_add_epi32(ph, offset)));
        _mm_storeu_ps(outL + i, _mm_add_ps(_mm_loadu_ps(outL + i), _mm_mul_ps(x, gl)));
        _mm_storeu_ps(outR + i, _mm_add_ps(_mm_loadu_ps(outR + i), _mm_mul_ps(x, gr)));
    }
    j.phase = p;
    j.inc = static_cast<std::uint32_t>(v);
    if (i < n) // the last few samples
        copyScalar(j, n - i, outL + i, outR + i);
}

/** Unison groups, one group of four copies per register, one lane per copy. */
void groupsSse(OscCopyJob* jobs, int count, int n, float* outL, float* outR) noexcept
{
    for (int g = 0; g + 4 <= count; g += 4)
    {
        OscCopyJob* j = jobs + g;
        const bool blending = j[0].blend > 0.0f || j[1].blend > 0.0f || j[2].blend > 0.0f || j[3].blend > 0.0f;
        const float* t0 = j[0].table0;
        const float* t1 = j[0].table1;
        const __m128i shift0 = _mm_cvtsi32_si128(32 - j[0].bits0);
        const __m128i shift1 = _mm_cvtsi32_si128(32 - j[0].bits1);
        const __m128i fracShift0 = _mm_cvtsi32_si128(j[0].bits0);
        const __m128i fracShift1 = _mm_cvtsi32_si128(j[0].bits1);
        const __m128 blendV = _mm_set_ps(j[3].blend, j[2].blend, j[1].blend, j[0].blend);
        const __m128 gl = _mm_set_ps(j[3].gainL, j[2].gainL, j[1].gainL, j[0].gainL);
        const __m128 gr = _mm_set_ps(j[3].gainR, j[2].gainR, j[1].gainR, j[0].gainR);
        const std::uint32_t pulseOffset = j[0].pulseOffset;
        const __m128i offset = _mm_set1_epi32(static_cast<int>(pulseOffset));
        const auto value = [&](__m128i ph) {
            __m128 x = catmullRom(ph, t0, shift0, fracShift0);
            if (blending)
                x = _mm_add_ps(x, _mm_mul_ps(blendV, _mm_sub_ps(catmullRom(ph, t1, shift1, fracShift1), x)));
            return x;
        };

        const auto lanes = [&](auto get) {
            return _mm_set_epi32(static_cast<int>(get(j[3])), static_cast<int>(get(j[2])), static_cast<int>(get(j[1])),
                                 static_cast<int>(get(j[0])));
        };
        __m128i p = lanes([](const OscCopyJob& c) { return c.phase; });
        __m128i v = lanes([](const OscCopyJob& c) { return c.inc; });
        const __m128i dv = lanes([](const OscCopyJob& c) { return c.step; });
        for (int i = 0; i < n; ++i)
        {
            v = _mm_add_epi32(v, dv);
            __m128 x = value(p);
            if (pulseOffset != 0)
                x = _mm_sub_ps(x, value(_mm_add_epi32(p, offset)));
            p = _mm_add_epi32(p, v);
            // Sum the four copies into the two channels.
            const __m128 l = _mm_mul_ps(x, gl);
            const __m128 r = _mm_mul_ps(x, gr);
            __m128 sum = _mm_add_ps(_mm_unpacklo_ps(l, r), _mm_unpackhi_ps(l, r)); // l02 r02 l13 r13
            sum = _mm_add_ps(sum, _mm_movehl_ps(sum, sum));
            alignas(16) float out[4];
            _mm_store_ps(out, sum);
            outL[i] += out[0];
            outR[i] += out[1];
        }
        alignas(16) std::uint32_t pOut[4], vOut[4];
        _mm_store_si128(reinterpret_cast<__m128i*>(pOut), p);
        _mm_store_si128(reinterpret_cast<__m128i*>(vOut), v);
        for (int k = 0; k < 4; ++k)
        {
            j[k].phase = pOut[k];
            j[k].inc = vOut[k];
        }
    }
}
#else
void groupsScalar(OscCopyJob* jobs, int count, int n, float* outL, float* outR) noexcept
{
    for (int k = 0; k < count; ++k)
        copyScalar(jobs[k], n, outL, outR);
}
#endif
} // namespace

OscillatorKernels oscillatorKernelsGeneric() noexcept
{
#if LATTICE_OSC_SSE2
    return {&groupsSse, &copySse};
#else
    return {&groupsScalar, &copyScalar};
#endif
}

OscillatorKernels oscillatorKernelsFor(SimdLevel level) noexcept
{
#if LATTICE_HAVE_AVX2_OSCILLATORS
    if (level == SimdLevel::Avx2)
        return oscillatorKernelsAvx2();
#endif
    static_cast<void>(level);
    return oscillatorKernelsGeneric();
}

} // namespace lattice::dsp
