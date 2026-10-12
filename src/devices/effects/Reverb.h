#pragma once

#include "devices/dsp/Biquad.h"
#include "devices/dsp/DelayLine.h"
#include "devices/dsp/FirHalfBand.h"
#include "devices/framework/StudioDevice.h"

#include <array>
#include <memory>
#include <vector>

namespace lattice
{

/** Audio effect "lattice.reverb": Lattice's own algorithmic reverb (docs/04-m3-devices.md, "Reverb
    design").

    Input -> pre-delay -> input diffusion (series all-passes) -> early reflections and late tail.
    - Early reflections: a multi-tap delay whose taps come from the room size and shape (room,
      hall, chamber, plate), panned across the stereo field and darkened with distance.
    - Late tail: a feedback delay network of 16 lines (8 in Eco, 32 in Ultra) mixed by a Householder
      matrix (every line feeds every other evenly, no favoured paths, no resonant cycles). Line
      lengths are mutually prime, scaled by the size, and each is modulated slightly and at random,
      read with interpolation, to break up metallic ringing.
    - Decay by frequency: each line has a three-band gain (low, mid, high with adjustable
      crossovers). Each band's gain per trip round a line is 10^(-3 L / (RT60 fs)), so every line
      loses 60 dB in exactly the band's decay time, whatever its length.
    - Width, high and low cut on the wet signal, freeze (the tail holds), dry/wet, early and late
      levels. Ultra runs the tail at twice the sample rate (through linear-phase half-bands).

    Every buffer is sized for the largest room in prepare(); changing the size glides the line
    lengths over about 300 ms, so there is no zipper noise and nothing is allocated. */
class ReverbDevice final : public StudioDevice
{
public:
    static constexpr const char* typeId = "lattice.reverb";
    static constexpr int maxLines = 32;
    static constexpr int numDiffusers = 6;
    static constexpr int numTaps = 24;
    static constexpr int earlyGroups = 4;

    enum class Quality : std::uint8_t
    {
        Eco,
        High,
        Ultra
    };

    enum class RoomShape : std::uint8_t
    {
        Room,
        Hall,
        Chamber,
        Plate
    };

    /** Parameter indices, in declaration order. (In a struct, so the names don't fill the
        class scope.) */
    struct Param
    {
        enum : std::uint8_t
        {
            size, decay, lowDecay, highDecay, lowCrossover, highCrossover,
            predelay, predelaySync, predelayBeats, diffusion, shape,
            earlyLevel, lateLevel, modulation, width, lowCut, highCut, freeze, quality, mix,
            numParams
        };
    };

    ReverbDevice();

    /** The decay time a band is set to (seconds): 0 low, 1 mid, 2 high (tests). */
    double bandDecay(int band) const noexcept;

protected:
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept override;
    void reset() noexcept override;

private:
    struct Line
    {
        dsp::DelayLine delay;
        double length = 1000.0;     // samples (at the tail's rate), gliding
        double targetLength = 1000.0;
        double modPhase = 0.0;
        double modRate = 0.3;       // Hz
        double modOffset = 0.0;     // the modulation now, in samples (stepped at control rate)
        double modStep = 0.0;
    };

    struct Diffuser
    {
        dsp::DelayLine delay;
        int length = 100;
    };

    struct Tap
    {
        int delay = 0;
        float gainL = 0.0f;
        float gainR = 0.0f;
        float damping = 0.0f; // one-pole coefficient (more for far taps)
    };

    void configure(bool snap) noexcept LATTICE_NONBLOCKING;
    /** One sample through the late tail at its own rate: in -> two outputs. */
    void tailTick(float in, float& outL, float& outR) noexcept LATTICE_NONBLOCKING;

    static constexpr int modulationBlock = 16;

    double tailRate = 48000.0;
    int modulationCountdown = 0;
    int numLines = 16;
    Quality currentQuality = Quality::High;
    std::array<Line, maxLines> lines;
    std::array<Diffuser, numDiffusers> diffusers;
    dsp::DelayLine predelayLine;
    dsp::DelayLine earlyLine;
    std::array<Tap, numTaps> taps{};
    float tapDampState[numTaps]{};
    alignas(16) float lowState[maxLines]{};  // crossover one-poles of each line
    alignas(16) float highState[maxLines]{};
    std::array<float, maxLines> lineGain{}; // per band: computed in configure
    std::array<float, maxLines> lowGain{};
    std::array<float, maxLines> highGain{};
    float lowCoef = 0.05f;
    float highCoef = 0.5f;
    float lastSize = -1.0f;
    int lastShape = -1;
    dsp::Biquad lowCutFilter[2];
    dsp::Biquad highCutFilter[2];
    float lastLowCut = -1.0f, lastHighCut = -1.0f;
    std::unique_ptr<dsp::FirHalfBand> upsampler, downsamplerL, downsamplerR;
    std::uint32_t random = 0x9e3779b9u;
    float mixValue = 0.3f;
};

} // namespace lattice
