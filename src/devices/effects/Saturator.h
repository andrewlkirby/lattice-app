#pragma once

#include "devices/dsp/FirHalfBand.h"
#include "devices/framework/StudioDevice.h"

#include <memory>
#include <vector>

namespace lattice
{

/** Audio effect "lattice.saturator" (docs/04-m3-devices.md, "Saturator"): soft (tanh), hard (clip),
    tube (asymmetric: even harmonics) or sine-fold curves, with drive, output level and dry/wet.

    The curve runs at 2x or 4x the sample rate through linear-phase FIR half-bands, which keeps
    its aliasing far down and the wet signal aligned with the dry one. The latency is the 4x path's
    (the 2x path is padded to match), so switching the factor never shifts the signal; it is
    reported for delay compensation. A DC blocker follows the tube curve. */
class SaturatorDevice final : public StudioDevice
{
public:
    static constexpr const char* typeId = "lattice.saturator";

    enum class Curve : std::uint8_t
    {
        Soft,
        Hard,
        Tube,
        SineFold
    };

    /** Parameter indices, in declaration order. (In a struct, so the names don't fill the
        class scope.) */
    struct Param
    {
        enum : std::uint8_t
        {
            curve, drive, output, oversampling, mix, numParams
        };
    };

    SaturatorDevice();

    /** The curve itself (tests). */
    static float shape(Curve c, float x) noexcept;

protected:
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept override;
    void reset() noexcept override;

private:
    struct Channel
    {
        std::unique_ptr<dsp::FirHalfBand> stageA; // base <-> 2x
        std::unique_ptr<dsp::FirHalfBand> stageB; // 2x <-> 4x
        float half = 0.0f;                        // one sample at 2x: aligns the 4x path
        std::vector<float> pad;                   // the 2x path's extra delay (base rate)
        int padPos = 0;
        std::vector<float> dry;                   // the dry signal, delayed by the latency
        int dryPos = 0;
        float dcIn = 0.0f, dcOut = 0.0f;
    };

    float wet4x(Channel& c, float x, Curve curveType, float drive) noexcept LATTICE_NONBLOCKING;
    float wet2x(Channel& c, float x, Curve curveType, float drive) noexcept LATTICE_NONBLOCKING;

    Channel channels[2];
    int latency = 0;
    int padLength = 0;
    float driveValue = 1.0f;
    float outputValue = 1.0f;
    float mixValue = 1.0f;
    float dcCoefficient = 0.9995f;
};

} // namespace lattice
