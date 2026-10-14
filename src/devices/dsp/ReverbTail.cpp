#include "devices/dsp/ReverbTail.h"

#include "devices/dsp/Simd.h"

#if defined(__SSE2__) || defined(_M_X64) || defined(_M_AMD64)
#define LATTICE_REVERB_SSE 1
#include <emmintrin.h>
#else
#define LATTICE_REVERB_SSE 0
#endif

namespace lattice::dsp
{

namespace
{
/** The sample after the newest: written at the end of each sample (also into the guard). */
inline void write(ReverbTailState& s, int line, std::uint32_t position, float value) noexcept
{
    const auto at = static_cast<std::int32_t>(position) & s.mask[line];
    s.memory[s.start[line] + at] = value;
    if (at < reverbTailGuard)
        s.memory[s.start[line] + s.mask[line] + 1 + at] = value;
}
} // namespace

#if LATTICE_REVERB_SSE
void reverbTailGeneric(ReverbTailState& s, const float* in, int n, float* outL, float* outR) noexcept
{
    const int lines = s.count;
    const __m128 third = _mm_set1_ps(1.0f / 3.0f), sixth = _mm_set1_ps(1.0f / 6.0f), half = _mm_set1_ps(0.5f);
    const __m128 lowC = _mm_set1_ps(s.lowCoef), highC = _mm_set1_ps(s.highCoef);
    const __m128 signs = _mm_setr_ps(1.0f, 1.0f, -1.0f, -1.0f);
    const __m128 leftWeights = _mm_setr_ps(1.0f, 0.0f, -1.0f, 0.0f);
    const __m128 rightWeights = _mm_setr_ps(0.0f, 1.0f, 0.0f, -1.0f);
    const __m128 feedbackScale = _mm_set1_ps(2.0f / static_cast<float>(lines));
    alignas(16) float outputs[reverbTailMaxLines];
    for (int i = 0; i < n; ++i)
    {
        // Each line's read and bands, four lines at a time.
        const __m128i readBase = _mm_set1_epi32(static_cast<std::int32_t>(s.writePos) - 2);
        __m128 sumV = _mm_setzero_ps();
        for (int g = 0; g < lines; g += 4)
        {
            const __m128 x = _mm_add_ps(_mm_load_ps(s.x + g), _mm_load_ps(s.step + g));
            _mm_store_ps(s.x + g, x);
            const __m128i t = _mm_cvttps_epi32(x);
            const __m128 frac = _mm_sub_ps(x, _mm_cvtepi32_ps(t));
            const __m128i whole = _mm_add_epi32(_mm_load_si128(reinterpret_cast<const __m128i*>(s.whole + g)), t);
            const __m128i at = _mm_add_epi32(
                _mm_and_si128(_mm_sub_epi32(readBase, whole), _mm_load_si128(reinterpret_cast<const __m128i*>(s.mask + g))),
                _mm_load_si128(reinterpret_cast<const __m128i*>(s.start + g)));
            alignas(16) std::int32_t a[4];
            _mm_store_si128(reinterpret_cast<__m128i*>(a), at);
            __m128 p0 = _mm_loadu_ps(s.memory + a[0]); // y2, y1, y0, ym1 (delays whole + 2 .. whole - 1)
            __m128 p1 = _mm_loadu_ps(s.memory + a[1]);
            __m128 p2 = _mm_loadu_ps(s.memory + a[2]);
            __m128 p3 = _mm_loadu_ps(s.memory + a[3]);
            _MM_TRANSPOSE4_PS(p0, p1, p2, p3);
            const __m128 y2 = p0, y1 = p1, y0 = p2, ym1 = p3;
            // Third-order Lagrange through x = -1, 0, 1, 2 (as DelayLine::read).
            const __m128 c1 = _mm_sub_ps(_mm_sub_ps(_mm_sub_ps(y1, _mm_mul_ps(ym1, third)), _mm_mul_ps(half, y0)),
                                         _mm_mul_ps(y2, sixth));
            const __m128 c2 = _mm_sub_ps(_mm_mul_ps(half, _mm_add_ps(ym1, y1)), y0);
            const __m128 c3 = _mm_add_ps(_mm_mul_ps(_mm_sub_ps(y2, ym1), sixth), _mm_mul_ps(half, _mm_sub_ps(y0, y1)));
            const __m128 raw =
                _mm_add_ps(_mm_mul_ps(_mm_add_ps(_mm_mul_ps(_mm_add_ps(_mm_mul_ps(c3, frac), c2), frac), c1), frac), y0);

            __m128 low = _mm_load_ps(s.low + g);
            low = _mm_add_ps(low, _mm_mul_ps(lowC, _mm_sub_ps(raw, low)));
            _mm_store_ps(s.low + g, low);
            const __m128 rest = _mm_sub_ps(raw, low);
            __m128 high = _mm_load_ps(s.high + g);
            high = _mm_add_ps(high, _mm_mul_ps(highC, _mm_sub_ps(rest, high)));
            _mm_store_ps(s.high + g, high);
            const __m128 out = _mm_add_ps(_mm_add_ps(_mm_mul_ps(_mm_load_ps(s.lowGain + g), low),
                                                     _mm_mul_ps(_mm_load_ps(s.midGain + g), high)),
                                          _mm_mul_ps(_mm_load_ps(s.highGain + g), _mm_sub_ps(rest, high)));
            _mm_store_ps(outputs + g, out);
            sumV = _mm_add_ps(sumV, out);
        }

        // The Householder feedback and the stereo sums.
        sumV = _mm_add_ps(sumV, _mm_movehl_ps(sumV, sumV));
        sumV = _mm_add_ss(sumV, _mm_shuffle_ps(sumV, sumV, 0x55));
        const __m128 feedback = _mm_mul_ps(feedbackScale, _mm_shuffle_ps(sumV, sumV, 0));
        const __m128 input = _mm_mul_ps(_mm_set1_ps(in[i]), signs);
        __m128 leftV = _mm_setzero_ps(), rightV = _mm_setzero_ps();
        const std::uint32_t next = s.writePos + 1;
        for (int g = 0; g < lines; g += 4)
        {
            const __m128 out = _mm_load_ps(outputs + g);
            leftV = _mm_add_ps(leftV, _mm_mul_ps(out, leftWeights));
            rightV = _mm_add_ps(rightV, _mm_mul_ps(out, rightWeights));
            alignas(16) float feed[4];
            _mm_store_ps(feed, _mm_add_ps(_mm_sub_ps(out, feedback), input));
            for (int j = 0; j < 4; ++j)
                write(s, g + j, next, feed[j]);
        }
        s.writePos = next;
        const __m128 lr = _mm_add_ps(_mm_unpacklo_ps(leftV, rightV), _mm_unpackhi_ps(leftV, rightV)); // l01 r01 l23 r23
        alignas(16) float sums[4];
        _mm_store_ps(sums, _mm_add_ps(lr, _mm_movehl_ps(lr, lr)));
        outL[i] = sums[0] * s.outputNorm;
        outR[i] = sums[1] * s.outputNorm;
    }
}
#else
void reverbTailGeneric(ReverbTailState& s, const float* in, int n, float* outL, float* outR) noexcept
{
    const int lines = s.count;
    float outputs[reverbTailMaxLines];
    for (int i = 0; i < n; ++i)
    {
        float sum = 0.0f;
        for (int l = 0; l < lines; ++l)
        {
            s.x[l] += s.step[l];
            const auto t = static_cast<std::int32_t>(s.x[l]);
            const float x = s.x[l] - static_cast<float>(t);
            const float* p = s.memory + s.start[l] +
                             ((static_cast<std::int32_t>(s.writePos) - 2 - (s.whole[l] + t)) & s.mask[l]);
            const float y2 = p[0], y1 = p[1], y0 = p[2], ym1 = p[3];
            // Third-order Lagrange through x = -1, 0, 1, 2 (as DelayLine::read).
            const float c1 = y1 - ym1 / 3.0f - 0.5f * y0 - y2 / 6.0f;
            const float c2 = 0.5f * (ym1 + y1) - y0;
            const float c3 = (y2 - ym1) / 6.0f + 0.5f * (y0 - y1);
            const float raw = ((c3 * x + c2) * x + c1) * x + y0;
            s.low[l] += s.lowCoef * (raw - s.low[l]);
            const float rest = raw - s.low[l];
            s.high[l] += s.highCoef * (rest - s.high[l]);
            outputs[l] = s.lowGain[l] * s.low[l] + s.midGain[l] * s.high[l] + s.highGain[l] * (rest - s.high[l]);
            sum += outputs[l];
        }
        const float k = 2.0f / static_cast<float>(lines) * sum;
        float left = 0.0f;
        float right = 0.0f;
        const std::uint32_t next = s.writePos + 1;
        for (int l = 0; l < lines; ++l)
        {
            const float sign = (l & 2) != 0 ? -1.0f : 1.0f;
            write(s, l, next, outputs[l] - k + in[i] * sign);
            if ((l & 1) == 0)
                left += sign * outputs[l];
            else
                right += sign * outputs[l];
        }
        s.writePos = next;
        outL[i] = left * s.outputNorm;
        outR[i] = right * s.outputNorm;
    }
}
#endif

ReverbTailKernel reverbTailKernelFor(SimdLevel level) noexcept
{
#if LATTICE_HAVE_AVX2_REVERB
    if (level == SimdLevel::Avx2)
        return &reverbTailAvx2;
#endif
    static_cast<void>(level);
    return &reverbTailGeneric;
}

} // namespace lattice::dsp
