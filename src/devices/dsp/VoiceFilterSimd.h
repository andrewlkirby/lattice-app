#pragma once

#include "core/RealtimeCheck.h"
#include "devices/dsp/Ladder.h"
#include "devices/dsp/Svf.h"
#include "devices/dsp/VoiceFilterKernels.h"

namespace lattice::dsp
{

/** One filter's work for a control block: its buffer (filtered in place), its filter object, and
    the cutoff coefficient g (ramping by gStep per sample) and resonance for the block. */
struct FilterLane
{
    float* buffer = nullptr;
    Svf* svf = nullptr;
    LadderFilter* ladder = nullptr;
    float g = 0.0f;
    float gStep = 0.0f;
    float resonance = 0.0f;
};

/** Runs the voice filters of several voices side by side, as many per register as the
    instruction set holds (4 with SSE2, 8 with AVX2), and two registers at a time where there are
    enough voices (docs/04-m3-devices.md, "Filters"; docs/perf/avx2-plan.md, step 2). A filter's
    feedback makes each one a serial chain; independent chains in the lanes of a register run in
    about the time of one.

    Coefficients follow each lane's g in steps of coefficientStep samples, as the scalar path does,
    and the results match the scalar filters to float rounding (the kernels use a refined
    reciprocal estimate where the scalar filters divide). The kernels are chosen in prepare(). */
class VoiceFilterSimd
{
public:
    static constexpr int coefficientStep = voiceFilterCoefficientStep;

    /** The generic kernels (SSE2 on x86-64) until prepare() picks others. */
    VoiceFilterSimd() noexcept;

    /** Picks the kernels for `level` (dsp/Simd.h). Not on the audio thread. */
    void prepare(SimdLevel level) noexcept;

    void svf(FilterLane* lanes, int count, int numSamples, bool steep, SvfMode mode,
             float inputGain) const noexcept LATTICE_NONBLOCKING;
    void ladder(FilterLane* lanes, int count, int numSamples, float drive) const noexcept LATTICE_NONBLOCKING;

    /** One lane on the filter objects themselves: the reference the kernels are tested against. */
    static void svfScalar(FilterLane& lane, int numSamples, bool steep, SvfMode mode,
                          float inputGain) noexcept LATTICE_NONBLOCKING;
    static void ladderScalar(FilterLane& lane, int numSamples, float drive) noexcept LATTICE_NONBLOCKING;

private:
    VoiceFilterKernels kernels;
};

} // namespace lattice::dsp
