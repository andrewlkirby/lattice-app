#pragma once

#include "core/RealtimeCheck.h"

#include "devices/dsp/VoiceFilterKernels.h"

#include <vector>

namespace lattice::dsp
{

/** Coefficients of a polyphase IIR half-band filter: two parallel chains of first-order all-pass
    sections (elliptic design, after Laurent de Soras' HIIR). Cheap and steep, with a small
    nonlinear phase in the passband. Used for 2x oversampling. */
struct HalfBandDesign
{
    std::vector<double> coefficients;

    /** attenuationDb in the stop band; transition is the transition width as a fraction of the
        (oversampled) sample rate, e.g. 0.05. */
    static HalfBandDesign make(double attenuationDb, double transition);
};

/** 2x up- and down-sampling with a half-band design of up to maxCoefficients sections. Fixed
    size, no allocation after construction. One channel. Its state is plain data (HalfBandState),
    which the voice filter kernels run on directly. */
class HalfBand2x
{
public:
    static constexpr int maxCoefficients = allpassMaxSections;

    explicit HalfBand2x(const HalfBandDesign& design);
    /** The design every voice filter uses: 80 dB down, transition 0.08 of the oversampled rate
        (flat to about 19.2 kHz at 48 kHz). */
    HalfBand2x();

    void reset() noexcept;

    /** One input sample to two output samples at twice the rate. */
    void upsample(float in, float& out0, float& out1) noexcept LATTICE_NONBLOCKING;
    /** Two input samples at twice the rate to one output sample. */
    float downsample(float in0, float in1) noexcept LATTICE_NONBLOCKING;

    int numCoefficients() const noexcept { return count; }

    HalfBandState& state() noexcept { return s; }

private:
    static float process(AllpassState& chain, float in) noexcept LATTICE_NONBLOCKING
    {
        for (int i = 0; i < chain.n; ++i)
        {
            const float out = chain.c[i] * (in - chain.y[i]) + chain.x[i];
            chain.x[i] = in;
            chain.y[i] = out;
            in = out;
        }
        return in;
    }

    HalfBandState s{};
    int count = 0;
};

} // namespace lattice::dsp
