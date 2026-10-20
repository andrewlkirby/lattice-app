#pragma once

#include "core/RtFifo.h"
#include "devices/dsp/Biquad.h"
#include "devices/framework/StudioDevice.h"

#include <array>
#include <cstdint>

namespace lattice
{

/** Audio effect "lattice.eq8" (docs/04-m3-devices.md, "EQ Eight").

    Eight bands, each off or one of: low cut 48 dB, low cut 12 dB, low shelf, bell, notch, high
    shelf, high cut 12 dB, high cut 48 dB; with frequency, gain and Q. Bells and cuts use the
    matched designs (BiquadCoefficients::design), so they keep their analog shape up to Nyquist.
    Each band works on both channels or, in mid/side, on the mid or the side.

    Band settings glide towards new values at control rate (every 32 samples) and the
    coefficients are recomputed there, so sweeping a band doesn't zipper. The audio after the EQ
    goes into a lock-free FIFO for the UI's spectrum analyzer (dropped when full). */
class EqEightDevice final : public StudioDevice
{
public:
    static constexpr const char* typeId = "lattice.eq8";
    static constexpr int numBands = 8;
    static constexpr int sectionsPerBand = 4; // 48 dB cuts
    static constexpr int controlBlock = 32;

    enum class Shape : std::uint8_t
    {
        LowCut48,
        LowCut12,
        LowShelf,
        Bell,
        Notch,
        HighShelf,
        HighCut12,
        HighCut48
    };

    enum class Target : std::uint8_t
    {
        Stereo,
        Mid,
        Side
    };

    /** Per band: on, shape, frequency, gain, Q, target. Then output gain and scale. */
    static int paramIndex(int band, int field) noexcept { return band * 6 + field; }
    static constexpr int outputGainParam = numBands * 6;
    static constexpr int scaleParam = numBands * 6 + 1;

    EqEightDevice();

    /** The response the EQ applies to a channel (target Stereo/Mid) at a frequency, with the
        current, settled settings (for tests and the UI curve). */
    std::complex<double> response(double frequency, Target channelTarget = Target::Stereo) const;

    /** UI thread: the analyzer's samples (mono sum after the EQ). */
    SpscFifo<float>& analyzer() noexcept { return analyzerFifo; }

protected:
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept override;
    void reset() noexcept override;

private:
    struct BandState
    {
        bool on = false;
        Shape shape = Shape::Bell;
        Target target = Target::Stereo;
        float frequency = 1000.0f; // smoothed values
        float gain = 0.0f;
        float q = 0.71f;
        int sections = 0;
        std::array<dsp::BiquadCoefficients, sectionsPerBand> coefficients{};
        std::array<std::array<dsp::Biquad, sectionsPerBand>, 2> filters{}; // per channel
        bool started = false;
    };

    static int designBand(Shape shape, double frequency, double gainDb, double q, double sampleRate,
                          std::array<dsp::BiquadCoefficients, sectionsPerBand>& out);
    void updateBand(int band, bool snap) noexcept LATTICE_NONBLOCKING;

    std::array<BandState, numBands> bands{};
    float outputGain = 1.0f;
    SpscFifo<float> analyzerFifo{1 << 14};
};

} // namespace lattice
