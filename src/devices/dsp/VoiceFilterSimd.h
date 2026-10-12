#pragma once

#include "core/RealtimeCheck.h"
#include "devices/dsp/Ladder.h"
#include "devices/dsp/Svf.h"

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

/** Runs the voice filters of several voices side by side, four per SSE register
    (docs/04-m3-devices.md, "Filters", and the performance charter: SIMD across voices). A filter's
    feedback makes each one a serial chain; four independent chains in the lanes of one register
    run in about the time of one.

    Coefficients follow each lane's g in steps of coefficientStep samples, as the scalar path does,
    and the results match the scalar filters (to float rounding). Falls back to the scalar filters
    where SSE2 isn't available. */
struct VoiceFilterSimd
{
    static constexpr int coefficientStep = 8;

    static void svf(FilterLane* lanes, int count, int numSamples, bool steep, SvfMode mode,
                    float inputGain) noexcept LATTICE_NONBLOCKING;
    static void ladder(FilterLane* lanes, int count, int numSamples, float drive) noexcept LATTICE_NONBLOCKING;

    /** The scalar versions (one lane), also used for the lanes left over. */
    static void svfScalar(FilterLane& lane, int numSamples, bool steep, SvfMode mode,
                          float inputGain) noexcept LATTICE_NONBLOCKING;
    static void ladderScalar(FilterLane& lane, int numSamples, float drive) noexcept LATTICE_NONBLOCKING;
};

} // namespace lattice::dsp
