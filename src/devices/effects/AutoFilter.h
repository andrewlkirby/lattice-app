#pragma once

#include "devices/dsp/DelayLine.h"
#include "devices/dsp/Ladder.h"
#include "devices/dsp/Lfo.h"
#include "devices/dsp/Svf.h"
#include "devices/framework/StudioDevice.h"

namespace lattice
{

/** Audio effect "lattice.autofilter" (docs/04-m3-devices.md, "Auto Filter"): a state-variable
    (low/high/band-pass, notch; 12 or 24 dB) or ladder filter whose cutoff moves with an LFO (free or
    tempo-synced, with a phase offset between the channels) and an envelope follower on the input.
    Drive and dry/wet. The cutoff is updated every 16 samples and the coefficients interpolated. */
class AutoFilterDevice final : public StudioDevice
{
public:
    static constexpr const char* typeId = "lattice.autofilter";
    static constexpr int controlBlock = 16;

    /** Parameter indices, in declaration order. (In a struct, so the names don't fill the
        class scope.) */
    struct Param
    {
        enum : std::uint8_t
        {
            type, cutoff, resonance, drive,
            lfoAmount, lfoShape, lfoRate, lfoSync, lfoSyncRate, lfoPhase,
            envAmount, envAttack, envRelease,
            mix, numParams
        };
    };

    AutoFilterDevice();

protected:
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept override;
    void reset() noexcept override;

private:
    dsp::Svf svf[2];
    dsp::LadderFilter ladder[2];
    dsp::Lfo lfo;
    dsp::LevelDetector follower;
    float g[2] = {-1.0f, -1.0f};
    float mixValue = 1.0f;
};

} // namespace lattice
