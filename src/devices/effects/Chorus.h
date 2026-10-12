#pragma once

#include "devices/dsp/DelayLine.h"
#include "devices/framework/StudioDevice.h"

namespace lattice
{

/** Audio effect "lattice.chorus" (docs/04-m3-devices.md, "Chorus"): two or three delay voices, each
    swept by its own phase of one LFO and spread across the stereo field; rate, depth, base delay,
    feedback, width and dry/wet. Delay reads are Lagrange-interpolated, so the sweep is smooth. */
class ChorusDevice final : public StudioDevice
{
public:
    static constexpr const char* typeId = "lattice.chorus";

    enum Param : int
    {
        voices, rate, depth, delay, feedback, width, mix, numParams
    };

    ChorusDevice();

protected:
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept override;
    void reset() noexcept override;

private:
    dsp::DelayLine lines[2];
    double phase = 0.0;
    float feedbackState[2] = {0.0f, 0.0f};
    float mixValue = 0.5f;
};

} // namespace lattice
