#pragma once

#include "core/RealtimeCheck.h"

#include <array>
#include <vector>

namespace lattice::dsp
{

struct VoiceFilterSimd;

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
    size, no allocation after construction. One channel. */
class HalfBand2x
{
public:
    friend struct VoiceFilterSimd; // processes four filters at once, on their state

    static constexpr int maxCoefficients = 16;

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

private:
    struct Chain
    {
        std::array<float, maxCoefficients> c{};
        std::array<float, maxCoefficients> x{}; // previous input of each section
        std::array<float, maxCoefficients> y{}; // previous output of each section
        int n = 0;

        float process(float in) noexcept LATTICE_NONBLOCKING
        {
            for (int i = 0; i < n; ++i)
            {
                const float out = c[static_cast<std::size_t>(i)] * (in - y[static_cast<std::size_t>(i)]) +
                                  x[static_cast<std::size_t>(i)];
                x[static_cast<std::size_t>(i)] = in;
                y[static_cast<std::size_t>(i)] = out;
                in = out;
            }
            return in;
        }
        void reset() noexcept
        {
            x.fill(0.0f);
            y.fill(0.0f);
        }
    };

    Chain upA, upB, downA, downB;
    float downDelay = 0.0f;
    int count = 0;
};

} // namespace lattice::dsp
