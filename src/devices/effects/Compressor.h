#pragma once

#include "devices/dsp/Biquad.h"
#include "devices/framework/StudioDevice.h"

#include <atomic>

namespace lattice
{

/** Audio effect "lattice.compressor" (docs/04-m3-devices.md, "Compressor").

    A feed-forward compressor: the detector (peak or RMS of the louder channel, or of the sidechain
    input) goes through a soft-knee gain computer in dB (Giannoulis, Massberg and Reiss, "Digital
    Dynamic Range Compressor Design", 2012) and a smoothed gain-reduction follower with separate
    attack and release. Makeup gain and dry/wet follow. The sidechain (another track's audio, 3e)
    can be filtered by the sidechain EQ before detection. */
class CompressorDevice final : public StudioDevice
{
public:
    static constexpr const char* typeId = "lattice.compressor";

    enum Param : int
    {
        threshold, ratio, attack, release, knee, makeup, mix, detection,
        sidechainOn, sidechainGain, eqOn, eqType, eqFrequency, eqQ, eqGain,
        numParams
    };

    CompressorDevice();

    /** The static curve: output level (dB) for an input level (dB), with the current settings. */
    float staticCurve(float inputDb) const noexcept;
    /** The current gain reduction in dB (<= 0), for meters. */
    float gainReductionDb() const noexcept { return reductionDb.load(std::memory_order_relaxed); }

protected:
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept override;
    void reset() noexcept override;

private:
    float reduction = 0.0f; // smoothed gain reduction in dB (<= 0)
    float rmsState = 0.0f;
    dsp::Biquad sidechainEq[2];
    float lastEqFrequency = -1.0f, lastEqQ = -1.0f, lastEqGain = -100.0f;
    int lastEqType = -1;
    float makeupGain = 1.0f;
    float mixValue = 1.0f;
    std::atomic<float> reductionDb{0.0f};
};

} // namespace lattice
