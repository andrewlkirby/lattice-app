#pragma once

// The voice filters' SIMD kernels (docs/perf/avx2-plan.md, step 2), in a plain-C-style interface so
// that VoiceFilterSimdAvx2.cpp, compiled for AVX2 + FMA, includes nothing else (see SincBlock.h):
// the filters' state is plain data here, owned by the C++ classes (Svf, LadderFilter, HalfBand2x),
// and the kernels work on it directly. No member functions or default member initialisers: an
// inline constructor compiled for AVX2 could be shared by the linker with code for any x86-64 CPU.

#include <cstdint>

namespace lattice::dsp
{

enum class SimdLevel : std::uint8_t; // devices/dsp/Simd.h (not included: see above)

enum class SvfMode : std::uint8_t
{
    LowPass,
    HighPass,
    BandPass,
    Notch
};

/** Coefficients follow each filter's g in steps of this many samples, in every path. */
constexpr int voiceFilterCoefficientStep = 8;

/** One 12 dB/octave TPT SVF stage's integrators. */
struct SvfStageState
{
    float ic1;
    float ic2;
};

constexpr int allpassMaxSections = 16;

/** A chain of first-order all-pass sections (one polyphase branch of a half-band filter). */
struct AllpassState
{
    float c[allpassMaxSections]; // coefficients
    float x[allpassMaxSections]; // previous input of each section
    float y[allpassMaxSections]; // previous output of each section
    int n;
};

/** 2x up- and down-sampling: two branches each way, and the down-sampler's one-sample delay. */
struct HalfBandState
{
    AllpassState upA, upB, downA, downB;
    float downDelay;
};

/** The ladder's four one-pole integrators and the previous sample's saturation argument. */
struct LadderState
{
    float s1, s2, s3, s4;
    float lastArgument;
};

/** One filter's work for a control block: its buffer (filtered in place) and state, and the cutoff
    coefficient g (ramping by gStep per sample) and damping or feedback k for the block. */
struct SvfJob
{
    float* buffer;
    SvfStageState* first; // used by the 24 dB filter only
    SvfStageState* second;
    float g, gStep;
    float k; // the second (or only) stage's 1/Q; the 24 dB filter's first stage is fixed
};

struct LadderJob
{
    float* buffer;
    LadderState* state;
    HalfBandState* oversampler; // every job's has the same coefficients
    float g, gStep;
    float k; // feedback, 4.1 x resonance
};

/** count jobs side by side, as many per register as the instruction set holds. Lanes left over
    are padded into a full register (copies of a real lane, discarded). */
using SvfKernel = void (*)(const SvfJob* jobs, int count, int numSamples, bool steep, float inputGain) noexcept;
/** outputScale: 1 / sqrt(inputGain), the level compensation for the drive. */
using LadderKernel = void (*)(const LadderJob* jobs, int count, int numSamples, float inputGain,
                              float outputScale) noexcept;

struct VoiceFilterKernels
{
    LadderKernel ladder;
    SvfKernel svf[4]; // by SvfMode
};

/** SSE2 on x86-64, plain C++ elsewhere. */
VoiceFilterKernels voiceFilterKernelsGeneric() noexcept;
#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__)
#define LATTICE_HAVE_AVX2_VOICE_FILTERS 1
/** AVX2 + FMA: only where the CPU has both (voiceFilterKernelsFor(SimdLevel::Avx2) gives them). */
VoiceFilterKernels voiceFilterKernelsAvx2() noexcept;
#else
#define LATTICE_HAVE_AVX2_VOICE_FILTERS 0
#endif

/** The kernels for a SIMD level (devices choose in prepare(): dsp/Simd.h). */
VoiceFilterKernels voiceFilterKernelsFor(SimdLevel level) noexcept;

} // namespace lattice::dsp
