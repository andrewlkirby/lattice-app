#include "devices/dsp/VoiceFilterSimd.h"

#include <algorithm>
#include <array>

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#define LATTICE_FILTER_SSE2 1
#include <emmintrin.h>
#include <xmmintrin.h>
#else
#define LATTICE_FILTER_SSE2 0
#endif

namespace lattice::dsp
{
namespace
{
#if LATTICE_FILTER_SSE2
constexpr float steepFirstK = 1.0f / 0.54119610f;

float steepSecondK(float resonance)
{
    return 1.0f / (1.30656296f * std::pow(36.0f, std::clamp(resonance, 0.0f, 1.0f)));
}

/** Loads one float from each of four lanes' buffers at index i. */
inline __m128 gather(float* const* buffers, int i) noexcept
{
    return _mm_set_ps(buffers[3][i], buffers[2][i], buffers[1][i], buffers[0][i]);
}

inline void scatter(float* const* buffers, int i, __m128 v) noexcept
{
    alignas(16) float out[4];
    _mm_store_ps(out, v);
    buffers[0][i] = out[0];
    buffers[1][i] = out[1];
    buffers[2][i] = out[2];
    buffers[3][i] = out[3];
}

template <typename Get>
inline __m128 lanes4(Get&& get) noexcept
{
    return _mm_set_ps(get(3), get(2), get(1), get(0));
}

template <typename Put>
inline void unlanes4(__m128 v, Put&& put) noexcept
{
    alignas(16) float out[4];
    _mm_store_ps(out, v);
    for (int l = 0; l < 4; ++l)
        put(l, out[l]);
}

struct SvfLaneState
{
    __m128 ic1, ic2;
};

template <SvfMode mode>
inline __m128 svfTick(__m128 x, SvfLaneState& s, __m128 a1, __m128 a2, __m128 a3, __m128 k) noexcept
{
    const __m128 v3 = _mm_sub_ps(x, s.ic2);
    const __m128 v1 = _mm_add_ps(_mm_mul_ps(a1, s.ic1), _mm_mul_ps(a2, v3));
    const __m128 v2 = _mm_add_ps(_mm_add_ps(s.ic2, _mm_mul_ps(a2, s.ic1)), _mm_mul_ps(a3, v3));
    s.ic1 = _mm_sub_ps(_mm_add_ps(v1, v1), s.ic1);
    s.ic2 = _mm_sub_ps(_mm_add_ps(v2, v2), s.ic2);
    if constexpr (mode == SvfMode::LowPass)
        return v2;
    else if constexpr (mode == SvfMode::HighPass)
        return _mm_sub_ps(_mm_sub_ps(x, _mm_mul_ps(k, v1)), v2);
    else if constexpr (mode == SvfMode::BandPass)
        return v1;
    else
        return _mm_sub_ps(x, _mm_mul_ps(k, v1));
}

template <SvfMode mode>
void svfGroup(FilterLane* const* lanes, int numSamples, bool steep, float inputGain) noexcept
{
    float* buffers[4] = {lanes[0]->buffer, lanes[1]->buffer, lanes[2]->buffer, lanes[3]->buffer};
    SvfLaneState first{lanes4([&](int l) { return lanes[l]->svf->stage(0).ic1; }),
                       lanes4([&](int l) { return lanes[l]->svf->stage(0).ic2; })};
    SvfLaneState second{lanes4([&](int l) { return lanes[l]->svf->stage(1).ic1; }),
                        lanes4([&](int l) { return lanes[l]->svf->stage(1).ic2; })};
    const __m128 k1 = _mm_set1_ps(steepFirstK);
    const __m128 k2 = lanes4([&](int l) {
        return steep ? steepSecondK(lanes[l]->resonance) : SvfCoefficients::dampingFor(lanes[l]->resonance);
    });
    const __m128 g0 = lanes4([&](int l) { return lanes[l]->g; });
    const __m128 gs = lanes4([&](int l) { return lanes[l]->gStep; });
    const __m128 one = _mm_set1_ps(1.0f);
    const __m128 gain = _mm_set1_ps(inputGain);

    for (int i0 = 0; i0 < numSamples; i0 += VoiceFilterSimd::coefficientStep)
    {
        const int i1 = std::min(numSamples, i0 + VoiceFilterSimd::coefficientStep);
        const __m128 g = _mm_add_ps(g0, _mm_mul_ps(gs, _mm_set1_ps(0.5f * static_cast<float>(i0 + i1))));
        const __m128 a21 = _mm_div_ps(one, _mm_add_ps(one, _mm_mul_ps(g, _mm_add_ps(g, k2))));
        const __m128 a22 = _mm_mul_ps(g, a21);
        const __m128 a23 = _mm_mul_ps(g, a22);
        const __m128 a11 = _mm_div_ps(one, _mm_add_ps(one, _mm_mul_ps(g, _mm_add_ps(g, k1))));
        const __m128 a12 = _mm_mul_ps(g, a11);
        const __m128 a13 = _mm_mul_ps(g, a12);
        for (int i = i0; i < i1; ++i)
        {
            __m128 x = _mm_mul_ps(gather(buffers, i), gain);
            if (steep)
                x = svfTick<mode>(x, first, a11, a12, a13, k1);
            scatter(buffers, i, svfTick<mode>(x, second, a21, a22, a23, k2));
        }
    }
    unlanes4(first.ic1, [&](int l, float v) { lanes[l]->svf->stage(0).ic1 = v; });
    unlanes4(first.ic2, [&](int l, float v) { lanes[l]->svf->stage(0).ic2 = v; });
    unlanes4(second.ic1, [&](int l, float v) { lanes[l]->svf->stage(1).ic1 = v; });
    unlanes4(second.ic2, [&](int l, float v) { lanes[l]->svf->stage(1).ic2 = v; });
}

/** A chain of first-order all-pass sections for four lanes (half-band polyphase path). */
struct AllpassChain4
{
    // Plain arrays: std::array<__m128> drops the vector type's alignment attributes (GCC warns).
    __m128 c[HalfBand2x::maxCoefficients];
    __m128 x[HalfBand2x::maxCoefficients];
    __m128 y[HalfBand2x::maxCoefficients];
    int n = 0;

    __m128 process(__m128 in) noexcept
    {
        for (int i = 0; i < n; ++i)
        {
            const __m128 out = _mm_add_ps(_mm_mul_ps(c[i], _mm_sub_ps(in, y[i])), x[i]);
            x[i] = in;
            y[i] = out;
            in = out;
        }
        return in;
    }
};
#endif
} // namespace

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
                          float inputGain) noexcept
{
    int done = 0;
#if LATTICE_FILTER_SSE2
    for (; done + 4 <= count; done += 4)
    {
        FilterLane* group[4] = {&lanes[done], &lanes[done + 1], &lanes[done + 2], &lanes[done + 3]};
        switch (mode)
        {
        case SvfMode::LowPass: svfGroup<SvfMode::LowPass>(group, numSamples, steep, inputGain); break;
        case SvfMode::HighPass: svfGroup<SvfMode::HighPass>(group, numSamples, steep, inputGain); break;
        case SvfMode::BandPass: svfGroup<SvfMode::BandPass>(group, numSamples, steep, inputGain); break;
        case SvfMode::Notch: svfGroup<SvfMode::Notch>(group, numSamples, steep, inputGain); break;
        }
    }
#endif
    for (; done < count; ++done)
        svfScalar(lanes[done], numSamples, steep, mode, inputGain);
}

void VoiceFilterSimd::ladder(FilterLane* lanes, int count, int numSamples, float drive) noexcept
{
    int done = 0;
#if LATTICE_FILTER_SSE2
    for (; done + 4 <= count; done += 4)
    {
        FilterLane* l4[4] = {&lanes[done], &lanes[done + 1], &lanes[done + 2], &lanes[done + 3]};
        float* buffers[4] = {l4[0]->buffer, l4[1]->buffer, l4[2]->buffer, l4[3]->buffer};
        const auto ladderOf = [&](int l) -> LadderFilter& { return *l4[l]->ladder; };

        // Gather the state: the ladder's integrators and the oversampler's all-pass chains.
        __m128 s1 = lanes4([&](int l) { return ladderOf(l).s1; });
        __m128 s2 = lanes4([&](int l) { return ladderOf(l).s2; });
        __m128 s3 = lanes4([&](int l) { return ladderOf(l).s3; });
        __m128 s4 = lanes4([&](int l) { return ladderOf(l).s4; });
        __m128 lastArgument = lanes4([&](int l) { return ladderOf(l).lastArgument; });
        __m128 downDelay = lanes4([&](int l) { return ladderOf(l).oversampler.downDelay; });
        AllpassChain4 chains[4];
        const auto chainOf = [](HalfBand2x& h, int which) -> HalfBand2x::Chain& {
            return which == 0 ? h.upA : which == 1 ? h.upB : which == 2 ? h.downA : h.downB;
        };
        for (int c = 0; c < 4; ++c)
        {
            const auto& reference = chainOf(ladderOf(0).oversampler, c);
            chains[c].n = reference.n;
            for (int i = 0; i < reference.n; ++i)
            {
                const auto si = static_cast<std::size_t>(i);
                chains[c].c[i] = _mm_set1_ps(reference.c[si]);
                chains[c].x[i] = lanes4([&](int l) { return chainOf(ladderOf(l).oversampler, c).x[si]; });
                chains[c].y[i] = lanes4([&](int l) { return chainOf(ladderOf(l).oversampler, c).y[si]; });
            }
        }

        const __m128 g0 = lanes4([&](int l) { return l4[l]->g; });
        const __m128 gs = lanes4([&](int l) { return l4[l]->gStep; });
        const __m128 k = lanes4([&](int l) { return 4.1f * std::clamp(l4[l]->resonance, 0.0f, 1.0f); });
        const float inputGain = std::max(drive, 0.01f);
        const __m128 inGain = _mm_set1_ps(inputGain);
        const __m128 outGain = _mm_mul_ps(_mm_add_ps(_mm_set1_ps(1.0f), _mm_mul_ps(_mm_set1_ps(0.5f), k)),
                                          _mm_set1_ps(1.0f / std::sqrt(inputGain)));
        const __m128 one = _mm_set1_ps(1.0f);
        const __m128 three = _mm_set1_ps(3.0f);
        const __m128 c27 = _mm_set1_ps(27.0f);
        const __m128 c9 = _mm_set1_ps(9.0f);
        const __m128 absMask = _mm_castsi128_ps(_mm_set1_epi32(0x7fffffff));
        const __m128 tiny = _mm_set1_ps(1.0e-4f);
        const __m128 half = _mm_set1_ps(0.5f);

        for (int i0 = 0; i0 < numSamples; i0 += coefficientStep)
        {
            const int i1 = std::min(numSamples, i0 + coefficientStep);
            const __m128 g = _mm_add_ps(g0, _mm_mul_ps(gs, _mm_set1_ps(0.5f * static_cast<float>(i0 + i1))));
            const __m128 G = _mm_div_ps(g, _mm_add_ps(one, g));
            const __m128 beta = _mm_div_ps(one, _mm_add_ps(one, g));
            const __m128 G2 = _mm_mul_ps(G, G);
            const __m128 G3 = _mm_mul_ps(G2, G);
            const __m128 G4 = _mm_mul_ps(G2, G2);

            const auto tick = [&](__m128 u) {
                // tanh(a) / a for the fast tanh: (27 + a^2) / (27 + 9 a^2) up to |a| = 3, then 1 / |a|.
                const __m128 a = lastArgument;
                const __m128 absA = _mm_and_ps(a, absMask);
                const __m128 a2 = _mm_mul_ps(a, a);
                const __m128 inner = _mm_div_ps(_mm_add_ps(c27, a2), _mm_add_ps(c27, _mm_mul_ps(c9, a2)));
                const __m128 outer = _mm_div_ps(one, _mm_max_ps(absA, tiny));
                const __m128 useInner = _mm_cmple_ps(absA, three);
                const __m128 t = _mm_or_ps(_mm_and_ps(useInner, inner), _mm_andnot_ps(useInner, outer));

                const __m128 S = _mm_mul_ps(beta, _mm_add_ps(_mm_add_ps(_mm_mul_ps(G3, s1), _mm_mul_ps(G2, s2)),
                                                             _mm_add_ps(_mm_mul_ps(G, s3), s4)));
                const __m128 G4t = _mm_mul_ps(G4, t);
                const __m128 y4 = _mm_div_ps(_mm_add_ps(_mm_mul_ps(G4t, u), S), _mm_add_ps(one, _mm_mul_ps(k, G4t)));
                const __m128 argument = _mm_sub_ps(u, _mm_mul_ps(k, y4));
                lastArgument = argument;
                const __m128 x0 = _mm_mul_ps(t, argument);

                __m128 v = _mm_mul_ps(_mm_sub_ps(x0, s1), G);
                const __m128 y1 = _mm_add_ps(v, s1);
                s1 = _mm_add_ps(y1, v);
                v = _mm_mul_ps(_mm_sub_ps(y1, s2), G);
                const __m128 y2 = _mm_add_ps(v, s2);
                s2 = _mm_add_ps(y2, v);
                v = _mm_mul_ps(_mm_sub_ps(y2, s3), G);
                const __m128 y3 = _mm_add_ps(v, s3);
                s3 = _mm_add_ps(y3, v);
                v = _mm_mul_ps(_mm_sub_ps(y3, s4), G);
                const __m128 out = _mm_add_ps(v, s4);
                s4 = _mm_add_ps(out, v);
                return out;
            };

            for (int i = i0; i < i1; ++i)
            {
                const __m128 x = _mm_mul_ps(gather(buffers, i), inGain);
                const __m128 up0 = chains[0].process(x);
                const __m128 up1 = chains[1].process(x);
                const __m128 ya = tick(up0);
                const __m128 yb = tick(up1);
                const __m128 da = chains[2].process(ya);
                const __m128 db = chains[3].process(downDelay);
                downDelay = yb;
                scatter(buffers, i, _mm_mul_ps(_mm_mul_ps(half, _mm_add_ps(da, db)), outGain));
            }
        }

        // Scatter the state back.
        unlanes4(s1, [&](int l, float v) { ladderOf(l).s1 = v; });
        unlanes4(s2, [&](int l, float v) { ladderOf(l).s2 = v; });
        unlanes4(s3, [&](int l, float v) { ladderOf(l).s3 = v; });
        unlanes4(s4, [&](int l, float v) { ladderOf(l).s4 = v; });
        unlanes4(lastArgument, [&](int l, float v) { ladderOf(l).lastArgument = v; });
        unlanes4(downDelay, [&](int l, float v) { ladderOf(l).oversampler.downDelay = v; });
        for (int c = 0; c < 4; ++c)
            for (int i = 0; i < chains[c].n; ++i)
            {
                const auto si = static_cast<std::size_t>(i);
                unlanes4(chains[c].x[i], [&](int l, float v) { chainOf(ladderOf(l).oversampler, c).x[si] = v; });
                unlanes4(chains[c].y[i], [&](int l, float v) { chainOf(ladderOf(l).oversampler, c).y[si] = v; });
            }
        // Keep the scalar coefficients in step, so a later scalar block continues seamlessly.
        for (int l = 0; l < 4; ++l)
            ladderOf(l).setCoefficients(l4[l]->g + l4[l]->gStep * static_cast<float>(numSamples), l4[l]->resonance, drive);
    }
#endif
    for (; done < count; ++done)
        ladderScalar(lanes[done], numSamples, drive);
}

} // namespace lattice::dsp
