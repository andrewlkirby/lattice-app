#pragma once

// The Reverb's late tail, a feedback delay network (docs/perf/avx2-plan.md, step 5), in a
// plain-C-style interface so that ReverbTailAvx2.cpp, compiled for AVX2 + FMA, includes nothing
// else (see SincBlock.h). No member functions or default member initialisers (see
// VoiceFilterKernels.h).

#include <cstdint>

namespace lattice::dsp
{

enum class SimdLevel : std::uint8_t; // devices/dsp/Simd.h (not included: see above)

constexpr int reverbTailMaxLines = 32;
/** Samples mirrored past the end of each line's memory, so a read's four points sit together. */
constexpr int reverbTailGuard = 3;

/** The tail's state, one array entry per line (structure of arrays, for SIMD).

    Each line's memory is a power of two (size = mask + 1) plus reverbTailGuard samples, which
    repeat its first ones. All lines are written together, so they share one write position; a
    line's newest sample is at start + (writePos & mask).

    Reads: before each sample, x += step; the line is read at delay whole + x (third-order
    Lagrange between the samples at delays whole + trunc(x) - 1 .. + 2), so a run of samples reads
    along a straight line. The device sets whole, x and step between runs so that x stays >= 0 and
    every delay stays within 1 .. mask - 3. */
struct ReverbTailState
{
    float* memory;
    std::int32_t count;      // lines in use: 8, 16 or 32
    std::uint32_t writePos;  // the newest sample, before each line's mask
    float lowCoef, highCoef; // the crossovers' one-pole coefficients
    float outputNorm;        // the stereo sums' scale
    alignas(32) std::int32_t start[reverbTailMaxLines]; // each line's memory, in memory
    alignas(32) std::int32_t mask[reverbTailMaxLines];
    alignas(32) std::int32_t whole[reverbTailMaxLines];
    alignas(32) float x[reverbTailMaxLines];
    alignas(32) float step[reverbTailMaxLines];
    alignas(32) float low[reverbTailMaxLines];  // the crossovers' states
    alignas(32) float high[reverbTailMaxLines];
    alignas(32) float lowGain[reverbTailMaxLines]; // per band, per trip round the line
    alignas(32) float midGain[reverbTailMaxLines];
    alignas(32) float highGain[reverbTailMaxLines];
};

/** n samples through the tail: in[i] -> outL[i], outR[i].

    Per sample, each line is read, split into three bands by two one-poles (below the low
    crossover, between, above the high one), each band scaled by its gain, and summed: the line's
    output. The outputs are mixed by a Householder matrix (each line gets its own output minus 2/N
    of them all) plus the input, with signs + + - - in each group of four lines, and written back.
    Lines alternate left and right in the stereo sums, with the same signs. */
using ReverbTailKernel = void (*)(ReverbTailState& s, const float* in, int n, float* outL, float* outR) noexcept;

/** SSE2 on x86-64 (four lines per register), plain C++ elsewhere. */
void reverbTailGeneric(ReverbTailState& s, const float* in, int n, float* outL, float* outR) noexcept;
#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__)
#define LATTICE_HAVE_AVX2_REVERB 1
/** AVX2 + FMA: eight lines per register. */
void reverbTailAvx2(ReverbTailState& s, const float* in, int n, float* outL, float* outR) noexcept;
#else
#define LATTICE_HAVE_AVX2_REVERB 0
#endif

/** The kernel for a SIMD level (devices choose in prepare(): dsp/Simd.h). */
ReverbTailKernel reverbTailKernelFor(SimdLevel level) noexcept;

} // namespace lattice::dsp
