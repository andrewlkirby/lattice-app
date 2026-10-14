#pragma once

// The Synth's oscillator kernels (docs/perf/avx2-plan.md, step 3), in a plain-C-style interface so
// that OscillatorSimdAvx2.cpp, compiled for AVX2 + FMA, includes nothing else (see SincBlock.h).
// A job is one oscillator copy over a block: raw pointers into a MipmappedWave's tables, so the
// kernels need nothing of the class. No member functions or default member initialisers (see
// VoiceFilterKernels.h).

#include <cstdint>

namespace lattice::dsp
{

enum class SimdLevel : std::uint8_t; // devices/dsp/Simd.h (not included: see above)

/** One oscillator copy for a block. Reads use Catmull-Rom interpolation, as MipmappedWave does. */
struct OscCopyJob
{
    const float* table0; // the level's table at x[-1] (levelData(level) + guard - 1)
    const float* table1; // the level blended into (or table0)
    std::int32_t bits0, bits1; // the tables' sizes, as log2
    float blend;               // 0: table0 only; towards 1, more of table1
    std::uint32_t pulseOffset; // != 0: a pulse, the wave minus itself shifted by this phase
    float gainL, gainR;        // added into the outputs times these
    std::int32_t step;         // the increment's change per sample (before each sample)
    std::uint32_t phase, inc;  // updated
};

/** Unison groups side by side: count is a multiple of 4, and each four consecutive jobs (a group)
    share their tables, sizes and pulse offset (each copy has its own blend, phase and gains). */
using OscGroupsKernel = void (*)(OscCopyJob* jobs, int count, int n, float* outL, float* outR) noexcept;
/** One copy, several samples at a time. */
using OscCopyKernel = void (*)(OscCopyJob& job, int n, float* outL, float* outR) noexcept;

struct OscillatorKernels
{
    OscGroupsKernel groups;
    OscCopyKernel copy;
};

/** SSE2 on x86-64 (four copies or four samples per register), plain C++ elsewhere. */
OscillatorKernels oscillatorKernelsGeneric() noexcept;
#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__)
#define LATTICE_HAVE_AVX2_OSCILLATORS 1
/** AVX2 + FMA: eight copies per register (two unison groups, each lane its own table), or one
    copy eight samples at a time. */
OscillatorKernels oscillatorKernelsAvx2() noexcept;
#else
#define LATTICE_HAVE_AVX2_OSCILLATORS 0
#endif

/** The kernels for a SIMD level (devices choose in prepare(): dsp/Simd.h). */
OscillatorKernels oscillatorKernelsFor(SimdLevel level) noexcept;

} // namespace lattice::dsp
