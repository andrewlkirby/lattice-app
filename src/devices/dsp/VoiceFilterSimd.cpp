#include "devices/dsp/VoiceFilterSimd.h"

#include "devices/dsp/Simd.h"

#include <algorithm>
#include <cmath>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#define LATTICE_FILTER_SSE2 1
#include <emmintrin.h>
#include <xmmintrin.h>
#else
#define LATTICE_FILTER_SSE2 0
#endif

#include "devices/dsp/VoiceFilterKernels.inl"

namespace lattice::dsp
{
namespace
{
#if LATTICE_FILTER_SSE2
/** Four lanes per SSE register (VoiceFilterKernels.inl). */
struct Sse
{
    using reg = __m128;
    using mask = __m128;
    static constexpr int width = 4;
    static constexpr int chunk = 4;

    static LATTICE_KERNEL_INLINE reg set1(float v) noexcept { return _mm_set1_ps(v); }
    static LATTICE_KERNEL_INLINE reg load(const float* p) noexcept { return _mm_load_ps(p); }
    static LATTICE_KERNEL_INLINE void store(float* p, reg v) noexcept { _mm_store_ps(p, v); }
    static LATTICE_KERNEL_INLINE reg add(reg a, reg b) noexcept { return _mm_add_ps(a, b); }
    static LATTICE_KERNEL_INLINE reg sub(reg a, reg b) noexcept { return _mm_sub_ps(a, b); }
    static LATTICE_KERNEL_INLINE reg mul(reg a, reg b) noexcept { return _mm_mul_ps(a, b); }
    static LATTICE_KERNEL_INLINE reg div(reg a, reg b) noexcept { return _mm_div_ps(a, b); }
    static LATTICE_KERNEL_INLINE reg madd(reg a, reg b, reg c) noexcept { return _mm_add_ps(_mm_mul_ps(a, b), c); }
    static LATTICE_KERNEL_INLINE reg nmadd(reg a, reg b, reg c) noexcept { return _mm_sub_ps(c, _mm_mul_ps(a, b)); }
    static LATTICE_KERNEL_INLINE reg max(reg a, reg b) noexcept { return _mm_max_ps(a, b); }
    static LATTICE_KERNEL_INLINE reg abs(reg a) noexcept
    {
        return _mm_and_ps(a, _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff)));
    }
    static LATTICE_KERNEL_INLINE reg reciprocal(reg d) noexcept
    {
        const reg r = _mm_rcp_ps(d); // 12 bits; one Newton-Raphson step makes about 23
        return _mm_mul_ps(r, _mm_sub_ps(_mm_set1_ps(2.0f), _mm_mul_ps(d, r)));
    }
    static LATTICE_KERNEL_INLINE mask lessEqual(reg a, reg b) noexcept { return _mm_cmple_ps(a, b); }
    static LATTICE_KERNEL_INLINE reg select(mask m, reg a, reg b) noexcept
    {
        return _mm_or_ps(_mm_and_ps(m, a), _mm_andnot_ps(m, b));
    }
    static LATTICE_KERNEL_INLINE reg gather(float* const* b, int i) noexcept
    {
        return _mm_set_ps(b[3][i], b[2][i], b[1][i], b[0][i]);
    }
    static LATTICE_KERNEL_INLINE void scatter(float* const* b, int i, reg v) noexcept
    {
        alignas(16) float t[4];
        _mm_store_ps(t, v);
        for (int l = 0; l < 4; ++l)
            b[l][i] = t[l];
    }
    static LATTICE_KERNEL_INLINE void loadBlock(float* const* b, int i, reg* out) noexcept
    {
        reg r0 = _mm_loadu_ps(b[0] + i), r1 = _mm_loadu_ps(b[1] + i), r2 = _mm_loadu_ps(b[2] + i),
            r3 = _mm_loadu_ps(b[3] + i);
        _MM_TRANSPOSE4_PS(r0, r1, r2, r3);
        out[0] = r0;
        out[1] = r1;
        out[2] = r2;
        out[3] = r3;
    }
    static LATTICE_KERNEL_INLINE void storeBlock(float* const* b, int i, const reg* in) noexcept
    {
        reg r0 = in[0], r1 = in[1], r2 = in[2], r3 = in[3];
        _MM_TRANSPOSE4_PS(r0, r1, r2, r3);
        _mm_storeu_ps(b[0] + i, r0);
        _mm_storeu_ps(b[1] + i, r1);
        _mm_storeu_ps(b[2] + i, r2);
        _mm_storeu_ps(b[3] + i, r3);
    }
};
using Generic = Sse;
#else
/** One lane, plain floats (builds without x86 SIMD). */
struct Scalar
{
    using reg = float;
    using mask = bool;
    static constexpr int width = 1;
    static constexpr int chunk = 1;

    static reg set1(float v) noexcept { return v; }
    static reg load(const float* p) noexcept { return *p; }
    static void store(float* p, reg v) noexcept { *p = v; }
    static reg add(reg a, reg b) noexcept { return a + b; }
    static reg sub(reg a, reg b) noexcept { return a - b; }
    static reg mul(reg a, reg b) noexcept { return a * b; }
    static reg div(reg a, reg b) noexcept { return a / b; }
    static reg madd(reg a, reg b, reg c) noexcept { return a * b + c; }
    static reg nmadd(reg a, reg b, reg c) noexcept { return c - a * b; }
    static reg max(reg a, reg b) noexcept { return std::max(a, b); }
    static reg abs(reg a) noexcept { return std::abs(a); }
    static reg reciprocal(reg d) noexcept { return 1.0f / d; }
    static mask lessEqual(reg a, reg b) noexcept { return a <= b; }
    static reg select(mask m, reg a, reg b) noexcept { return m ? a : b; }
    static reg gather(float* const* b, int i) noexcept { return b[0][i]; }
    static void scatter(float* const* b, int i, reg v) noexcept { b[0][i] = v; }
    static void loadBlock(float* const* b, int i, reg* out) noexcept { out[0] = b[0][i]; }
    static void storeBlock(float* const* b, int i, const reg* in) noexcept { b[0][i] = in[0]; }
};
using Generic = Scalar;
#endif

/** Jobs are built on the stack, this many at a time. */
constexpr int maxJobs = 64;
} // namespace

VoiceFilterKernels voiceFilterKernelsGeneric() noexcept
{
    return kernelsFor<Generic>();
}

VoiceFilterKernels voiceFilterKernelsFor(SimdLevel level) noexcept
{
#if LATTICE_HAVE_AVX2_VOICE_FILTERS
    if (level == SimdLevel::Avx2)
        return voiceFilterKernelsAvx2();
#endif
    static_cast<void>(level);
    return voiceFilterKernelsGeneric();
}

VoiceFilterSimd::VoiceFilterSimd() noexcept : kernels(voiceFilterKernelsGeneric()) {}

void VoiceFilterSimd::prepare(SimdLevel level) noexcept
{
    kernels = voiceFilterKernelsFor(level);
}

void VoiceFilterSimd::svfScalar(FilterLane& lane, int numSamples, bool steep, SvfMode mode, float inputGain) noexcept
{
    for (int i0 = 0; i0 < numSamples; i0 += coefficientStep)
    {
        const int i1 = std::min(numSamples, i0 + coefficientStep);
        lane.svf->setCoefficients(lane.g + lane.gStep * 0.5f * static_cast<float>(i0 + i1), lane.resonance, steep);
        lane.svf->processBlock(lane.buffer + i0, i1 - i0, inputGain, mode);
    }
}

void VoiceFilterSimd::ladderScalar(FilterLane& lane, int numSamples, float drive) noexcept
{
    for (int i0 = 0; i0 < numSamples; i0 += coefficientStep)
    {
        const int i1 = std::min(numSamples, i0 + coefficientStep);
        lane.ladder->setCoefficients(lane.g + lane.gStep * 0.5f * static_cast<float>(i0 + i1), lane.resonance, drive);
        for (int i = i0; i < i1; ++i)
            lane.buffer[i] = lane.ladder->process(lane.buffer[i]);
    }
}

void VoiceFilterSimd::svf(FilterLane* lanes, int count, int numSamples, bool steep, SvfMode mode,
                          float inputGain) const noexcept
{
    SvfJob jobs[maxJobs];
    for (int first = 0; first < count; first += maxJobs)
    {
        const int n = std::min(maxJobs, count - first);
        for (int j = 0; j < n; ++j)
        {
            const auto& lane = lanes[first + j];
            const float r = std::clamp(lane.resonance, 0.0f, 1.0f);
            // The 24 dB filter's resonant stage: Q from 1.307 (Butterworth) up by a factor of 36.
            const float k = steep ? 1.0f / (1.30656296f * std::pow(36.0f, r)) : SvfCoefficients::dampingFor(r);
            jobs[j] = {lane.buffer, &lane.svf->stage(0), &lane.svf->stage(1), lane.g, lane.gStep, k};
        }
        kernels.svf[static_cast<int>(mode)](jobs, n, numSamples, steep, inputGain);
    }
}

void VoiceFilterSimd::ladder(FilterLane* lanes, int count, int numSamples, float drive) const noexcept
{
    const float inputGain = std::max(drive, 0.01f);
    const float outputScale = 1.0f / std::sqrt(inputGain);
    LadderJob jobs[maxJobs];
    for (int first = 0; first < count; first += maxJobs)
    {
        const int n = std::min(maxJobs, count - first);
        for (int j = 0; j < n; ++j)
        {
            auto& lane = lanes[first + j];
            jobs[j] = {lane.buffer, &lane.ladder->state(), &lane.ladder->oversamplerState(), lane.g, lane.gStep,
                       4.1f * std::clamp(lane.resonance, 0.0f, 1.0f)};
        }
        kernels.ladder(jobs, n, numSamples, inputGain, outputScale);
    }
}

} // namespace lattice::dsp
