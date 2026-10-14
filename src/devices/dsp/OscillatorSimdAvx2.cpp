// Compiled with AVX2 + FMA (CMakeLists.txt sets the flags for *Avx2.cpp on x86-64). Include nothing
// but the plain interface and the intrinsics: see OscillatorKernels.h.

#include "devices/dsp/OscillatorKernels.h"

#if LATTICE_HAVE_AVX2_OSCILLATORS
#include <immintrin.h>

// MSVC doesn't inline lambdas or small functions reliably in these loops (a call per sample
// costs more than the work), so the helpers are forced inline.
#if defined(_MSC_VER) && !defined(__clang__)
#define LATTICE_OSC_INLINE __forceinline
#else
#define LATTICE_OSC_INLINE inline __attribute__((always_inline))
#endif

namespace lattice::dsp
{
namespace
{
/** Catmull-Rom from the four points around each lane's position (x[-1] .. x[2]). */
LATTICE_OSC_INLINE __m256 catmullRom(__m256 xm1, __m256 x0, __m256 x1, __m256 x2, __m256 frac) noexcept
{
    const __m256 half = _mm256_set1_ps(0.5f);
    const __m256 c1 = _mm256_mul_ps(half, _mm256_sub_ps(x1, xm1));
    const __m256 c2 = _mm256_fnmadd_ps(half, x2, _mm256_fnmadd_ps(_mm256_set1_ps(2.5f), x0, _mm256_add_ps(xm1, _mm256_add_ps(x1, x1))));
    const __m256 c3 = _mm256_fmadd_ps(half, _mm256_sub_ps(x2, xm1), _mm256_mul_ps(_mm256_set1_ps(1.5f), _mm256_sub_ps(x0, x1)));
    return _mm256_fmadd_ps(_mm256_fmadd_ps(_mm256_fmadd_ps(c3, frac, c2), frac, c1), frac, x0);
}

/** The fraction below the index: the bits under it, as a 31-bit value (the conversion is signed). */
LATTICE_OSC_INLINE __m256 fractionOf(__m256i shiftedUp) noexcept
{
    return _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_srli_epi32(shiftedUp, 1)), _mm256_set1_ps(4.6566128730773926e-10f));
}

/** Loads the four points at each lane's index from that lane's table and interpolates. The
    hardware gather is slow on the reference CPU (Zen 3), so: eight 4-float loads, paired into
    256-bit registers (lanes l and l + 4) and transposed within each half. */
LATTICE_OSC_INLINE __m256 readLanes(const float* const* tables, __m256i index, __m256 frac) noexcept
{
    // The indices straight from the register (a store and eight reloads waits on store forwarding).
    // Two indices per 64-bit move (indices are below 2^31, so the halves need no sign handling).
    const __m128i lo = _mm256_castsi256_si128(index);
    const __m128i hi = _mm256_extracti128_si256(index, 1);
    const auto i01 = static_cast<std::uint64_t>(_mm_cvtsi128_si64(lo));
    const auto i23 = static_cast<std::uint64_t>(_mm_extract_epi64(lo, 1));
    const auto i45 = static_cast<std::uint64_t>(_mm_cvtsi128_si64(hi));
    const auto i67 = static_cast<std::uint64_t>(_mm_extract_epi64(hi, 1));
    const auto low = [](std::uint64_t v) { return static_cast<std::uint32_t>(v); };
    const auto high = [](std::uint64_t v) { return static_cast<std::uint32_t>(v >> 32); };
    const auto pair = [&](int l, std::uint32_t a, std::uint32_t b) {
        return _mm256_insertf128_ps(_mm256_castps128_ps256(_mm_loadu_ps(tables[l] + a)),
                                    _mm_loadu_ps(tables[l + 4] + b), 1);
    };
    const __m256 r0 = pair(0, low(i01), low(i45));
    const __m256 r1 = pair(1, high(i01), high(i45));
    const __m256 r2 = pair(2, low(i23), low(i67));
    const __m256 r3 = pair(3, high(i23), high(i67));
    const __m256 t0 = _mm256_unpacklo_ps(r0, r1), t1 = _mm256_unpacklo_ps(r2, r3);
    const __m256 t2 = _mm256_unpackhi_ps(r0, r1), t3 = _mm256_unpackhi_ps(r2, r3);
    const __m256 xm1 = _mm256_shuffle_ps(t0, t1, _MM_SHUFFLE(1, 0, 1, 0));
    const __m256 x0 = _mm256_shuffle_ps(t0, t1, _MM_SHUFFLE(3, 2, 3, 2));
    const __m256 x1 = _mm256_shuffle_ps(t2, t3, _MM_SHUFFLE(1, 0, 1, 0));
    const __m256 x2 = _mm256_shuffle_ps(t2, t3, _MM_SHUFFLE(3, 2, 3, 2));
    return catmullRom(xm1, x0, x1, x2, frac);
}

/** Per-sample sums over the 8 lanes of 8 registers: out[s] = sum of v[s]'s lanes. */
LATTICE_OSC_INLINE __m256 laneSums(const __m256* v) noexcept
{
    const __m256 h01 = _mm256_hadd_ps(v[0], v[1]), h23 = _mm256_hadd_ps(v[2], v[3]);
    const __m256 h45 = _mm256_hadd_ps(v[4], v[5]), h67 = _mm256_hadd_ps(v[6], v[7]);
    const __m256 q0 = _mm256_hadd_ps(h01, h23); // samples 0-3: lanes 0-3 | lanes 4-7
    const __m256 q1 = _mm256_hadd_ps(h45, h67); // samples 4-7
    return _mm256_add_ps(_mm256_permute2f128_ps(q0, q1, 0x20), _mm256_permute2f128_ps(q0, q1, 0x31));
}

LATTICE_OSC_INLINE void addInto(float* out, __m256 v, int count) noexcept
{
    if (count == 8)
    {
        _mm256_storeu_ps(out, _mm256_add_ps(_mm256_loadu_ps(out), v));
        return;
    }
    alignas(32) float t[8];
    _mm256_store_ps(t, v);
    for (int s = 0; s < count; ++s)
        out[s] += t[s];
}

/** One register of copies: their tables, and the phases and increments as they run. */
struct CopyLanes
{
    const float* t0[8];
    const float* t1[8];
    __m256i shift0, shift1, bits0, bits1, offset;
    __m256i p, v, dv;
    __m256 pulseOn, blend;
    bool blending, pulse;

    LATTICE_OSC_INLINE __m256 value(__m256i at) const noexcept
    {
        __m256 x = readLanes(t0, _mm256_srlv_epi32(at, shift0), fractionOf(_mm256_sllv_epi32(at, bits0)));
        if (blending)
        {
            const __m256 y = readLanes(t1, _mm256_srlv_epi32(at, shift1), fractionOf(_mm256_sllv_epi32(at, bits1)));
            x = _mm256_fmadd_ps(blend, _mm256_sub_ps(y, x), x);
        }
        return x;
    }

    /** The next sample of every lane. */
    LATTICE_OSC_INLINE __m256 next() noexcept
    {
        v = _mm256_add_epi32(v, dv);
        __m256 x = value(p);
        if (pulse)
            x = _mm256_sub_ps(x, _mm256_and_ps(pulseOn, value(_mm256_add_epi32(p, offset))));
        p = _mm256_add_epi32(p, v);
        return x;
    }
};

/** Two unison groups per register, one lane per copy, each lane reading its own tables (per-lane
    shifts give each its own table size). The lanes' outputs are summed into the channels eight
    samples at a time. A lone group fills half a register; the other half repeats its first copy
    at zero gain, and its state is not written back. */
void groupsAvx2(OscCopyJob* jobs, int count, int n, float* outL, float* outR) noexcept
{
    for (int first = 0; first < count; first += 8)
    {
        const int real = count - first < 8 ? count - first : 8;
        CopyLanes c;
        alignas(32) std::int32_t sh0[8], sh1[8], b0[8], b1[8], off[8], pulseMask[8], ph[8], inc[8], step[8];
        alignas(32) float blend[8], gl[8], gr[8];
        bool blending = false, pulse = false, stereo = false;
        for (int l = 0; l < 8; ++l)
        {
            const bool isReal = l < real;
            const OscCopyJob& j = jobs[first + (isReal ? l : 0)];
            c.t0[l] = j.table0;
            c.t1[l] = j.table1;
            sh0[l] = 32 - j.bits0;
            sh1[l] = 32 - j.bits1;
            b0[l] = j.bits0;
            b1[l] = j.bits1;
            off[l] = static_cast<std::int32_t>(j.pulseOffset);
            pulseMask[l] = j.pulseOffset != 0 ? -1 : 0;
            ph[l] = static_cast<std::int32_t>(j.phase);
            inc[l] = static_cast<std::int32_t>(j.inc);
            step[l] = j.step;
            blend[l] = j.blend;
            gl[l] = isReal ? j.gainL : 0.0f;
            gr[l] = isReal ? j.gainR : 0.0f;
            blending = blending || j.blend > 0.0f;
            pulse = pulse || j.pulseOffset != 0;
            stereo = stereo || gr[l] != 0.0f;
        }
        const auto load = [](const std::int32_t* a) { return _mm256_load_si256(reinterpret_cast<const __m256i*>(a)); };
        c.shift0 = load(sh0);
        c.shift1 = load(sh1);
        c.bits0 = load(b0);
        c.bits1 = load(b1);
        c.offset = load(off);
        c.pulseOn = _mm256_castsi256_ps(load(pulseMask));
        c.blend = _mm256_load_ps(blend);
        c.p = load(ph);
        c.v = load(inc);
        c.dv = load(step);
        c.blending = blending;
        c.pulse = pulse;
        const __m256 gainL = _mm256_load_ps(gl), gainR = _mm256_load_ps(gr);

        __m256 l[8], r[8];
        int i = 0;
        for (; i + 8 <= n; i += 8)
        {
            for (int s = 0; s < 8; ++s)
            {
                const __m256 x = c.next();
                l[s] = _mm256_mul_ps(x, gainL);
                r[s] = _mm256_mul_ps(x, gainR);
            }
            addInto(outL + i, laneSums(l), 8);
            if (stereo)
                addInto(outR + i, laneSums(r), 8);
        }
        if (i < n)
        {
            const int left = n - i;
            for (int s = 0; s < 8; ++s)
            {
                if (s < left)
                {
                    const __m256 x = c.next();
                    l[s] = _mm256_mul_ps(x, gainL);
                    r[s] = _mm256_mul_ps(x, gainR);
                }
                else
                {
                    l[s] = r[s] = _mm256_setzero_ps();
                }
            }
            addInto(outL + i, laneSums(l), left);
            if (stereo)
                addInto(outR + i, laneSums(r), left);
        }

        _mm256_store_si256(reinterpret_cast<__m256i*>(ph), c.p);
        _mm256_store_si256(reinterpret_cast<__m256i*>(inc), c.v);
        for (int k = 0; k < real; ++k)
        {
            jobs[first + k].phase = static_cast<std::uint32_t>(ph[k]);
            jobs[first + k].inc = static_cast<std::uint32_t>(inc[k]);
        }
    }
}

/** Eight points of one table around eight positions. */
LATTICE_OSC_INLINE __m256 readOne(const float* table, __m256i at, __m128i shift, __m128i bits) noexcept
{
    const float* tables[8] = {table, table, table, table, table, table, table, table};
    return readLanes(tables, _mm256_srl_epi32(at, shift), fractionOf(_mm256_sll_epi32(at, bits)));
}

/** One copy, eight samples at a time. The eight phases come in closed form: with the increment
    ramping by `step` before each sample, sample s of a step reads p + s v + step s (s + 1) / 2. */
void copyAvx2(OscCopyJob& j, int n, float* outL, float* outR) noexcept
{
    const bool blending = j.blend > 0.0f;
    const __m128i shift0 = _mm_cvtsi32_si128(32 - j.bits0), shift1 = _mm_cvtsi32_si128(32 - j.bits1);
    const __m128i bits0 = _mm_cvtsi32_si128(j.bits0), bits1 = _mm_cvtsi32_si128(j.bits1);
    const __m256 blendV = _mm256_set1_ps(j.blend);
    const __m256 gainL = _mm256_set1_ps(j.gainL), gainR = _mm256_set1_ps(j.gainR);
    const __m256i offset = _mm256_set1_epi32(static_cast<int>(j.pulseOffset));
    const __m256i s = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
    const __m256i triangle = _mm256_setr_epi32(0, 1, 3, 6, 10, 15, 21, 28);

    std::uint32_t p = j.phase;
    std::uint32_t v = j.inc;
    const auto st = static_cast<std::uint32_t>(j.step);
    for (int i = 0; i < n; i += 8)
    {
        const int count = n - i < 8 ? n - i : 8;
        const __m256i at = _mm256_add_epi32(
            _mm256_set1_epi32(static_cast<int>(p)),
            _mm256_add_epi32(_mm256_mullo_epi32(s, _mm256_set1_epi32(static_cast<int>(v))),
                             _mm256_mullo_epi32(triangle, _mm256_set1_epi32(static_cast<int>(st)))));
        __m256 x = readOne(j.table0, at, shift0, bits0);
        if (blending)
            x = _mm256_fmadd_ps(blendV, _mm256_sub_ps(readOne(j.table1, at, shift1, bits1), x), x);
        if (j.pulseOffset != 0)
        {
            const __m256i shifted = _mm256_add_epi32(at, offset);
            __m256 y = readOne(j.table0, shifted, shift0, bits0);
            if (blending)
                y = _mm256_fmadd_ps(blendV, _mm256_sub_ps(readOne(j.table1, shifted, shift1, bits1), y), y);
            x = _mm256_sub_ps(x, y);
        }
        addInto(outL + i, _mm256_mul_ps(x, gainL), count);
        addInto(outR + i, _mm256_mul_ps(x, gainR), count);
        const auto c = static_cast<std::uint32_t>(count);
        p += c * v + st * (c * (c + 1) / 2);
        v += c * st;
    }
    j.phase = p;
    j.inc = v;
}
} // namespace

OscillatorKernels oscillatorKernelsAvx2() noexcept
{
    return {&groupsAvx2, &copyAvx2};
}

} // namespace lattice::dsp
#endif
