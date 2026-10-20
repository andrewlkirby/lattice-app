// Compiled with AVX2 + FMA (CMakeLists.txt sets the flags for *Avx2.cpp on x86-64). Include nothing
// but the plain interface and the intrinsics: see SincBlock.h.

#include "devices/dsp/ReverbTail.h"

#if LATTICE_HAVE_AVX2_REVERB
#include <immintrin.h>

namespace lattice::dsp
{

namespace
{
/** The sum of a register's eight lanes, in lane 0. */
inline __m128 laneSum(__m256 v) noexcept
{
    __m128 s = _mm_add_ps(_mm256_castps256_ps128(v), _mm256_extractf128_ps(v, 1));
    s = _mm_add_ps(s, _mm_movehl_ps(s, s));
    return _mm_add_ss(s, _mm_shuffle_ps(s, s, 0x55));
}

/** Two 4-float loads as one register: `lo` in lanes 0-3, `hi` in 4-7. */
inline __m256 loadPair(const float* lo, const float* hi) noexcept
{
    return _mm256_insertf128_ps(_mm256_castps128_ps256(_mm_loadu_ps(lo)), _mm_loadu_ps(hi), 1);
}
} // namespace

void reverbTailAvx2(ReverbTailState& s, const float* in, int n, float* outL, float* outR) noexcept
{
    const int lines = s.count;
    const __m256 third = _mm256_set1_ps(1.0f / 3.0f), sixth = _mm256_set1_ps(1.0f / 6.0f),
                 half = _mm256_set1_ps(0.5f);
    const __m256 lowC = _mm256_set1_ps(s.lowCoef), highC = _mm256_set1_ps(s.highCoef);
    const __m256 signs = _mm256_setr_ps(1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, -1.0f, -1.0f);
    const __m256 leftWeights = _mm256_setr_ps(1.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, -1.0f, 0.0f);
    const __m256 rightWeights = _mm256_setr_ps(0.0f, 1.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f, -1.0f);
    const __m256 feedbackScale = _mm256_set1_ps(2.0f / static_cast<float>(lines));
    const __m256i guard = _mm256_set1_epi32(reverbTailGuard);
    const __m256i one = _mm256_set1_epi32(1);
    alignas(32) float outputs[reverbTailMaxLines];
    for (int i = 0; i < n; ++i)
    {
        // Each line's read and bands, eight lines at a time.
        const __m256i readBase = _mm256_set1_epi32(static_cast<std::int32_t>(s.writePos) - 2);
        __m256 sumV = _mm256_setzero_ps();
        for (int g = 0; g < lines; g += 8)
        {
            const __m256 x = _mm256_add_ps(_mm256_load_ps(s.x + g), _mm256_load_ps(s.step + g));
            _mm256_store_ps(s.x + g, x);
            const __m256i t = _mm256_cvttps_epi32(x);
            const __m256 frac = _mm256_sub_ps(x, _mm256_cvtepi32_ps(t));
            const __m256i whole =
                _mm256_add_epi32(_mm256_load_si256(reinterpret_cast<const __m256i*>(s.whole + g)), t);
            const __m256i at = _mm256_add_epi32(
                _mm256_and_si256(_mm256_sub_epi32(readBase, whole),
                                 _mm256_load_si256(reinterpret_cast<const __m256i*>(s.mask + g))),
                _mm256_load_si256(reinterpret_cast<const __m256i*>(s.start + g)));
            alignas(32) std::int32_t a[8];
            _mm256_store_si256(reinterpret_cast<__m256i*>(a), at);
            // Lines 0-3 in the low half, 4-7 in the high: y2, y1, y0, ym1 (delays whole + 2 ..
            // whole - 1), transposed within each half.
            const __m256 p0 = loadPair(s.memory + a[0], s.memory + a[4]);
            const __m256 p1 = loadPair(s.memory + a[1], s.memory + a[5]);
            const __m256 p2 = loadPair(s.memory + a[2], s.memory + a[6]);
            const __m256 p3 = loadPair(s.memory + a[3], s.memory + a[7]);
            const __m256 t0 = _mm256_unpacklo_ps(p0, p1), t1 = _mm256_unpacklo_ps(p2, p3);
            const __m256 t2 = _mm256_unpackhi_ps(p0, p1), t3 = _mm256_unpackhi_ps(p2, p3);
            const __m256 y2 = _mm256_shuffle_ps(t0, t1, 0x44), y1 = _mm256_shuffle_ps(t0, t1, 0xee);
            const __m256 y0 = _mm256_shuffle_ps(t2, t3, 0x44), ym1 = _mm256_shuffle_ps(t2, t3, 0xee);
            // Third-order Lagrange through x = -1, 0, 1, 2 (as DelayLine::read).
            const __m256 c1 = _mm256_sub_ps(_mm256_fnmadd_ps(half, y0, _mm256_fnmadd_ps(ym1, third, y1)),
                                            _mm256_mul_ps(y2, sixth));
            const __m256 c2 = _mm256_fmsub_ps(half, _mm256_add_ps(ym1, y1), y0);
            const __m256 c3 = _mm256_fmadd_ps(_mm256_sub_ps(y2, ym1), sixth, _mm256_mul_ps(half, _mm256_sub_ps(y0, y1)));
            const __m256 raw = _mm256_fmadd_ps(_mm256_fmadd_ps(_mm256_fmadd_ps(c3, frac, c2), frac, c1), frac, y0);

            __m256 low = _mm256_load_ps(s.low + g);
            low = _mm256_fmadd_ps(lowC, _mm256_sub_ps(raw, low), low);
            _mm256_store_ps(s.low + g, low);
            const __m256 rest = _mm256_sub_ps(raw, low);
            __m256 high = _mm256_load_ps(s.high + g);
            high = _mm256_fmadd_ps(highC, _mm256_sub_ps(rest, high), high);
            _mm256_store_ps(s.high + g, high);
            const __m256 out = _mm256_fmadd_ps(
                _mm256_load_ps(s.lowGain + g), low,
                _mm256_fmadd_ps(_mm256_load_ps(s.midGain + g), high,
                                _mm256_mul_ps(_mm256_load_ps(s.highGain + g), _mm256_sub_ps(rest, high))));
            _mm256_store_ps(outputs + g, out);
            sumV = _mm256_add_ps(sumV, out);
        }

        // The Householder feedback and the stereo sums; each line's write (and its guard copy).
        const __m256 feedback = _mm256_mul_ps(feedbackScale, _mm256_broadcastss_ps(laneSum(sumV)));
        const __m256 input = _mm256_mul_ps(_mm256_set1_ps(in[i]), signs);
        __m256 leftV = _mm256_setzero_ps(), rightV = _mm256_setzero_ps();
        const std::uint32_t next = s.writePos + 1;
        const __m256i nextV = _mm256_set1_epi32(static_cast<std::int32_t>(next));
        for (int g = 0; g < lines; g += 8)
        {
            const __m256 out = _mm256_load_ps(outputs + g);
            leftV = _mm256_fmadd_ps(out, leftWeights, leftV);
            rightV = _mm256_fmadd_ps(out, rightWeights, rightV);
            alignas(32) float feed[8];
            _mm256_store_ps(feed, _mm256_add_ps(_mm256_sub_ps(out, feedback), input));
            const __m256i mask = _mm256_load_si256(reinterpret_cast<const __m256i*>(s.mask + g));
            const __m256i at = _mm256_and_si256(nextV, mask);
            const __m256i start = _mm256_load_si256(reinterpret_cast<const __m256i*>(s.start + g));
            alignas(32) std::int32_t a[8];
            _mm256_store_si256(reinterpret_cast<__m256i*>(a), _mm256_add_epi32(start, at));
            for (int j = 0; j < 8; ++j)
                s.memory[a[j]] = feed[j];
            const int guarded = _mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(guard, at)));
            if (guarded != 0)
            {
                // Within the first samples of some line's memory: copy past its end too.
                _mm256_store_si256(reinterpret_cast<__m256i*>(a),
                                   _mm256_add_epi32(_mm256_add_epi32(start, at), _mm256_add_epi32(mask, one)));
                for (int j = 0; j < 8; ++j)
                    if ((guarded & (1 << j)) != 0)
                        s.memory[a[j]] = feed[j];
            }
        }
        s.writePos = next;
        outL[i] = _mm_cvtss_f32(laneSum(leftV)) * s.outputNorm;
        outR[i] = _mm_cvtss_f32(laneSum(rightV)) * s.outputNorm;
    }
    _mm256_zeroupper();
}

} // namespace lattice::dsp

#endif // LATTICE_HAVE_AVX2_REVERB
