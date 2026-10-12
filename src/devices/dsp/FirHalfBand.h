#pragma once

#include "core/RealtimeCheck.h"

#include <vector>

namespace lattice::dsp
{

/** A linear-phase FIR half-band filter for 2x up- and down-sampling (docs/04-m3-devices.md,
    "Oversampling"): a Kaiser-windowed sinc of `taps` = 4k + 3 coefficients, cut off at a quarter
    of the oversampled rate. Linear phase keeps processed and dry signals aligned, so dry/wet mixes
    don't comb-filter. Half the taps are zero and the centre is 1/2, which the polyphase form
    skips: each output costs about taps / 2 multiplies.

    Up then down delays the signal by (taps - 1) / 2 samples at the lower rate. */
class FirHalfBand
{
public:
    /** Not on the audio thread. attenuationDb sets the Kaiser window. */
    FirHalfBand(int taps, double attenuationDb);

    void reset() noexcept;

    /** One sample in, two out at twice the rate. */
    void upsample(float in, float& out0, float& out1) noexcept LATTICE_NONBLOCKING;
    /** Two samples in at twice the rate, one out. */
    float downsample(float in0, float in1) noexcept LATTICE_NONBLOCKING;

    // --- Blocks: the same filter over whole blocks, SIMD across outputs (much less overhead per
    // sample). An instance is used either sample by sample or in blocks, not both.
    /** Not on the audio thread: room for blocks of up to maxInput samples at the lower rate. */
    void prepareBlocks(int maxInput);
    /** n samples in, 2n out (interleaved in time: out[2m], out[2m + 1]). */
    void upsampleBlock(const float* in, int n, float* out) noexcept LATTICE_NONBLOCKING;
    /** 2n samples in at twice the rate, n out. */
    void downsampleBlock(const float* in, int n, float* out) noexcept LATTICE_NONBLOCKING;

    int taps() const noexcept { return numTaps; }
    /** Round-trip latency (up then down) in samples at the lower rate. */
    int roundTripLatency() const noexcept { return (numTaps - 1) / 2; }
    /** The filter's coefficients (tests). */
    const std::vector<double>& coefficients() const noexcept { return h; }

private:
    int numTaps;
    int centre;
    std::vector<double> h;
    std::vector<float> evenTaps; // h[0], h[2], ... (the non-zero ones)
    std::vector<float> reversed; // the same, oldest-first and zero-padded to a multiple of 4 (SIMD)
    // Histories (twice the length, written twice, so reads never wrap)
    std::vector<float> upHistory;
    int upPos = 0;
    std::vector<float> downEven;
    std::vector<float> downOdd;
    int downPos = 0;
    int length = 0; // history length per branch

    // Block state: the last `historyLength` inputs, then the block, in one linear buffer per branch.
    int historyLength = 0;
    std::vector<float> blockUp;
    std::vector<float> blockEven;
    std::vector<float> blockOdd;
};

} // namespace lattice::dsp
