#pragma once

#include "devices/dsp/DelayLine.h"
#include "devices/framework/StudioDevice.h"

namespace lattice
{

/** Audio effect "lattice.utility" (docs/04-m3-devices.md, "Utility"): gain, balance, stereo width
    (mid/side), mono, polarity per channel and a DC filter. Gains ramp over 20 ms; with nothing
    moving, the math is exact (no smoothing residue). */
class UtilityDevice final : public StudioDevice
{
public:
    static constexpr const char* typeId = "lattice.utility";

    enum Param : int
    {
        gain, balance, width, mono, invertLeft, invertRight, dcFilter, numParams
    };

    UtilityDevice();

    /** The left/right matrix and gains for the current settings (also the test's reference):
        out = [ll lr; rl rr] * in. */
    struct Matrix
    {
        float ll = 1.0f, lr = 0.0f, rl = 0.0f, rr = 1.0f;
    };
    Matrix currentMatrix() const noexcept;

protected:
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept override;
    void reset() noexcept override;

private:
    Matrix matrix;  // in effect now
    Matrix target;  // where it is ramping to
    Matrix step;    // per sample
    int remaining = 0;
    int rampSamples = 960;
    float dcCoefficient = 0.9995f;
    float dcIn[2] = {0.0f, 0.0f};
    float dcOut[2] = {0.0f, 0.0f};
    bool started = false;
};

} // namespace lattice
