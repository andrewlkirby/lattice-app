#pragma once

// Which SIMD instruction set the DSP kernels use (docs/perf/avx2-plan.md, "Infrastructure").
//
// Kernels come in versions per level: SSE2 (any x86-64), and AVX2 + FMA in *Avx2.cpp files, which
// CMake compiles with those flags (and outside link-time optimisation) and which include only
// intrinsics and plain-C-style headers. Devices pick their kernels in prepare() from simdLevel(),
// so the audio thread never checks the CPU.

#include <cstdint>

namespace lattice::dsp
{

enum class SimdLevel : std::uint8_t
{
    Scalar, // no x86 SIMD (ARM builds): plain C++ loops
    Sse2,   // every x86-64 CPU
    Avx2    // AVX2 with FMA
};

/** What this CPU and build support. */
SimdLevel cpuSimdLevel() noexcept;

/** The level the kernels use: the CPU's, or SSE2 if the environment variable LATTICE_SIMD says
    "sse2" (for comparing paths), or as set by setSimdLevel(). */
SimdLevel simdLevel() noexcept;

/** Message thread (or before audio starts): uses `level`, or the CPU's if that is lower (and never
    below SSE2 on x86-64). Devices prepared afterwards pick their kernels for it. */
void setSimdLevel(SimdLevel level) noexcept;

const char* simdLevelName(SimdLevel level) noexcept;

/** Sets a level for a scope (tests, benchmarks) and restores the previous one. */
class ScopedSimdLevel
{
public:
    explicit ScopedSimdLevel(SimdLevel level) noexcept : previous(simdLevel()) { setSimdLevel(level); }
    ~ScopedSimdLevel() { setSimdLevel(previous); }
    ScopedSimdLevel(const ScopedSimdLevel&) = delete;
    ScopedSimdLevel& operator=(const ScopedSimdLevel&) = delete;

private:
    SimdLevel previous;
};

} // namespace lattice::dsp
