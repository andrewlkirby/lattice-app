#pragma once

#include "core/RealtimeCheck.h"
#include "devices/dsp/Wavetable.h"

#include <cstdint>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#define LATTICE_OSC_SSE2 1
#include <emmintrin.h>
#include <xmmintrin.h>
#else
#define LATTICE_OSC_SSE2 0
#endif

namespace lattice::dsp
{

#if LATTICE_OSC_SSE2
namespace detail
{
/** Catmull-Rom reads of four phases from one level's table (table points at x[-1]). */
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
} // namespace detail
#endif

/** One oscillator copy over a block, four samples at a time with SSE2 (docs/04-m3-devices.md,
    "Oscillators": the inner loops are SIMD).

    Reads `wave` at one table position (level and blend into the next level), with the increment
    ramping by `step` per sample (inc += step before each sample, then phase += inc, as in the scalar
    loop, so both give the same phases). pulseOffset != 0 makes it a pulse: the wave minus itself
    shifted by that phase. Adds the output to outL / outR times gainL / gainR. Updates phase and
    inc. Returns false where SSE2 isn't available (the caller then runs its scalar loop). */
inline bool renderCopy(const MipmappedWave& wave, const MipmappedWave::Position& at, std::uint32_t& phase,
                       std::uint32_t& inc, std::int32_t step, std::uint32_t pulseOffset, float gainL, float gainR,
                       float* outL, float* outR, int n) noexcept LATTICE_NONBLOCKING
{
#if LATTICE_OSC_SSE2
    const bool blending = at.blend > 0.0f && at.level + 1 < MipmappedWave::numLevels;
    const int nextLevel = blending ? at.level + 1 : at.level;
    const float* t0 = wave.levelData(at.level) + MipmappedWave::guard - 1;
    const float* t1 = wave.levelData(nextLevel) + MipmappedWave::guard - 1;
    const int bits0 = wave.levelBits(at.level);
    const int bits1 = wave.levelBits(nextLevel);
    const __m128i shift0 = _mm_cvtsi32_si128(32 - bits0);
    const __m128i shift1 = _mm_cvtsi32_si128(32 - bits1);
    const __m128i fracShift0 = _mm_cvtsi32_si128(bits0);
    const __m128i fracShift1 = _mm_cvtsi32_si128(bits1);
    const __m128 fracScale = _mm_set1_ps(4.6566128730773926e-10f); // 2^-31
    const __m128 blendV = _mm_set1_ps(at.blend);
    const __m128 gl = _mm_set1_ps(gainL);
    const __m128 gr = _mm_set1_ps(gainR);
    const __m128 half = _mm_set1_ps(0.5f);
    const __m128 onePointFive = _mm_set1_ps(1.5f);
    const __m128 two = _mm_set1_ps(2.0f);
    const __m128 twoPointFive = _mm_set1_ps(2.5f);
    const __m128i offset = _mm_set1_epi32(static_cast<int>(pulseOffset));

    const auto readAt = [&](__m128i ph, const float* table, __m128i shift, __m128i fracShift) {
        const __m128i index = _mm_srl_epi32(ph, shift);
        // The fraction: the bits below the index, as a 31-bit value (the conversion is signed).
        const __m128 frac = _mm_mul_ps(_mm_cvtepi32_ps(_mm_srli_epi32(_mm_sll_epi32(ph, fracShift), 1)), fracScale);
        alignas(16) std::int32_t idx[4];
        _mm_store_si128(reinterpret_cast<__m128i*>(idx), index);
        __m128 a = _mm_loadu_ps(table + idx[0]);
        __m128 b = _mm_loadu_ps(table + idx[1]);
        __m128 c = _mm_loadu_ps(table + idx[2]);
        __m128 d = _mm_loadu_ps(table + idx[3]);
        _MM_TRANSPOSE4_PS(a, b, c, d); // a = x[-1], b = x[0], c = x[1], d = x[2] in each lane
        // Catmull-Rom
        const __m128 c1 = _mm_mul_ps(half, _mm_sub_ps(c, a));
        const __m128 c2 = _mm_sub_ps(_mm_add_ps(_mm_sub_ps(a, _mm_mul_ps(twoPointFive, b)), _mm_mul_ps(two, c)),
                                     _mm_mul_ps(half, d));
        const __m128 c3 = _mm_add_ps(_mm_mul_ps(half, _mm_sub_ps(d, a)), _mm_mul_ps(onePointFive, _mm_sub_ps(b, c)));
        return _mm_add_ps(_mm_mul_ps(_mm_add_ps(_mm_mul_ps(_mm_add_ps(_mm_mul_ps(c3, frac), c2), frac), c1), frac), b);
    };
    const auto value = [&](__m128i ph) {
        __m128 x = readAt(ph, t0, shift0, fracShift0);
        if (blending)
            x = _mm_add_ps(x, _mm_mul_ps(blendV, _mm_sub_ps(readAt(ph, t1, shift1, fracShift1), x)));
        return x;
    };

    std::uint32_t p = phase;
    auto v = static_cast<std::int64_t>(inc);
    int i = 0;
    for (; i + 4 <= n; i += 4)
    {
        alignas(16) std::uint32_t phases[4];
        for (int k = 0; k < 4; ++k)
        {
            v += step;
            phases[k] = p;
            p += static_cast<std::uint32_t>(v);
        }
        const __m128i ph = _mm_load_si128(reinterpret_cast<const __m128i*>(phases));
        __m128 x = value(ph);
        if (pulseOffset != 0)
            x = _mm_sub_ps(x, value(_mm_add_epi32(ph, offset)));
        _mm_storeu_ps(outL + i, _mm_add_ps(_mm_loadu_ps(outL + i), _mm_mul_ps(x, gl)));
        _mm_storeu_ps(outR + i, _mm_add_ps(_mm_loadu_ps(outR + i), _mm_mul_ps(x, gr)));
    }
    for (; i < n; ++i) // the last few samples
    {
        v += step;
        float x = at.blend > 0.0f ? wave.sample(p, at) : wave.readLevel(at.level, p);
        if (pulseOffset != 0)
            x -= at.blend > 0.0f ? wave.sample(p + pulseOffset, at) : wave.readLevel(at.level, p + pulseOffset);
        p += static_cast<std::uint32_t>(v);
        outL[i] += x * gainL;
        outR[i] += x * gainR;
    }
    phase = p;
    inc = static_cast<std::uint32_t>(v);
    return true;
#else
    juce::ignoreUnused(wave, at, phase, inc, step, pulseOffset, gainL, gainR, outL, outR, n);
    return false;
#endif
}

/** Four copies of one oscillator (unison voices) at once with SSE2, one lane per copy. All four read
    the same table level, each blending into the next level by its own amount. Increments ramp by
    `step` per sample. Adds each copy times its gains to outL / outR. Returns false where SSE2 isn't
    available. */
inline bool renderFourCopies(const MipmappedWave& wave, int level, const float blend[4], std::uint32_t phase[4],
                             std::uint32_t inc[4], const std::int32_t step[4], std::uint32_t pulseOffset,
                             const float gainL[4], const float gainR[4], float* outL, float* outR,
                             int n) noexcept LATTICE_NONBLOCKING
{
#if LATTICE_OSC_SSE2
    const bool blending = blend[0] > 0.0f || blend[1] > 0.0f || blend[2] > 0.0f || blend[3] > 0.0f;
    const int nextLevel = level + 1 < MipmappedWave::numLevels ? level + 1 : level;
    const float* t0 = wave.levelData(level) + MipmappedWave::guard - 1;
    const float* t1 = wave.levelData(nextLevel) + MipmappedWave::guard - 1;
    const int bits0 = wave.levelBits(level);
    const int bits1 = wave.levelBits(nextLevel);
    const __m128i shift0 = _mm_cvtsi32_si128(32 - bits0);
    const __m128i shift1 = _mm_cvtsi32_si128(32 - bits1);
    const __m128i fracShift0 = _mm_cvtsi32_si128(bits0);
    const __m128i fracShift1 = _mm_cvtsi32_si128(bits1);
    const __m128 blendV = _mm_loadu_ps(blend);
    const __m128 gl = _mm_loadu_ps(gainL);
    const __m128 gr = _mm_loadu_ps(gainR);
    const __m128i offset = _mm_set1_epi32(static_cast<int>(pulseOffset));
    const auto value = [&](__m128i ph) {
        __m128 x = detail::catmullRom(ph, t0, shift0, fracShift0);
        if (blending)
            x = _mm_add_ps(x, _mm_mul_ps(blendV, _mm_sub_ps(detail::catmullRom(ph, t1, shift1, fracShift1), x)));
        return x;
    };

    __m128i p = _mm_loadu_si128(reinterpret_cast<const __m128i*>(phase));
    __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i*>(inc));
    const __m128i dv = _mm_loadu_si128(reinterpret_cast<const __m128i*>(step));
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
    _mm_storeu_si128(reinterpret_cast<__m128i*>(phase), p);
    _mm_storeu_si128(reinterpret_cast<__m128i*>(inc), v);
    return true;
#else
    juce::ignoreUnused(wave, level, blend, phase, inc, step, pulseOffset, gainL, gainR, outL, outR, n);
    return false;
#endif
}

} // namespace lattice::dsp
