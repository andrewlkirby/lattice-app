#pragma once

// The sinc interpolator's inner loop over a block of positions, in a plain-C-style interface so
// that SincBlockAvx2.cpp, compiled for AVX2 + FMA, includes nothing else: no inline functions or
// templates that the linker could share with code meant for any x86-64 CPU.

#include <cstdint>

namespace lattice::dsp
{

enum class SimdLevel : std::uint8_t; // devices/dsp/Simd.h (not included: see above)

/** One kernel level, as the block interpolator needs it. */
struct SincKernelView
{
    const float* rows = nullptr; // (phases + 1) rows of `stride` weights
    int taps = 0;
    int stride = 0; // a multiple of 8
    int phases = 0;
};

/** For each i in [0, n): interpolates channels 0 and 1 (s1 may equal s0 for mono) at position
    index[i] + fraction[i] into outL[i] and outR[i]. Every index[i] - taps/2 + 1 .. index[i] +
    stride - taps/2 must be readable. */
using SincBlockFunction = void (*)(const SincKernelView& k, const float* s0, const float* s1,
                                   const std::int64_t* index, const float* fraction, int n, float* outL,
                                   float* outR) noexcept;

/** SSE (any x86-64) or plain C++. */
void sincBlockGeneric(const SincKernelView& k, const float* s0, const float* s1, const std::int64_t* index,
                      const float* fraction, int n, float* outL, float* outR) noexcept;
#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__)
#define LATTICE_HAVE_AVX2_KERNELS 1
/** AVX2 + FMA: only where the CPU has both (sincBlockFor(SimdLevel::Avx2) gives it). */
void sincBlockAvx2(const SincKernelView& k, const float* s0, const float* s1, const std::int64_t* index,
                   const float* fraction, int n, float* outL, float* outR) noexcept;
#else
#define LATTICE_HAVE_AVX2_KERNELS 0
#endif

/** The implementation for a SIMD level (devices choose in prepare(): dsp/Simd.h). */
SincBlockFunction sincBlockFor(SimdLevel level) noexcept;
/** The implementation for the current simdLevel(). */
SincBlockFunction sincBlock() noexcept;

/** Cubic Hermite (Catmull-Rom) over a block of positions, the Sampler's "Draft" quality: four
    positions at a time with SSE. index[i] - 1 .. index[i] + 2 must be readable. */
void hermiteBlock(const float* s0, const float* s1, const std::int64_t* index, const float* fraction, int n,
                  float* outL, float* outR) noexcept;

} // namespace lattice::dsp
