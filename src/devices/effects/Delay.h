#pragma once

#include "devices/dsp/Biquad.h"
#include "devices/dsp/DelayLine.h"
#include "devices/framework/StudioDevice.h"

namespace lattice
{

/** Audio effect "lattice.delay" (docs/04-m3-devices.md, "Delay"): stereo or ping-pong; each side's
    time synced to the tempo or free in milliseconds; feedback through high- and low-cut filters;
    modulation of the delay time; freeze (the buffer loops, no new input); dry/wet.

    A time change glides over about 100 ms (the pitch bends, as on tape). Unmodulated and settled,
    the delay is an exact whole number of samples wherever the time falls on one (any synced value
    at 120 BPM and 48 kHz). Up to 20 s. */
class DelayDevice final : public StudioDevice
{
public:
    static constexpr const char* typeId = "lattice.delay";
    static constexpr double maxSeconds = 20.0;

    /** Parameter indices, in declaration order. (In a struct, so the names don't fill the
        class scope.) */
    struct Param
    {
        enum : std::uint8_t
        {
            mode, sync, syncLeft, syncRight, msLeft, msRight, link,
            feedback, lowCut, highCut, modRate, modDepth, freeze, mix,
            numParams
        };
    };

    DelayDevice();

protected:
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept override;
    void reset() noexcept override;

private:
    dsp::DelayLine lines[2];
    dsp::Biquad lowCutFilter[2];
    dsp::Biquad highCutFilter[2];
    float lastLowCut = -1.0f, lastHighCut = -1.0f;
    double delay[2] = {0.0, 0.0}; // current (gliding) delay in samples
    bool started = false;
    double lfoPhase = 0.0;
    float mixValue = 0.5f;
};

} // namespace lattice
