#pragma once

#include "core/RealtimeCheck.h"

#include <cmath>
#include <cstdint>

namespace lattice::dsp
{

enum class LfoShape : std::uint8_t
{
    Sine,
    Triangle,
    SawUp,
    SawDown,
    Square,
    SampleAndHold
};

/** A low-frequency oscillator, evaluated at control rate (docs/04-m3-devices.md, "LFO"). Output in
    -1..1. Free-running at a rate in Hz, or synced to the transport: then its phase is computed from
    the beat position, so it stays locked to the bar however blocks fall. */
class Lfo
{
public:
    static double phaseFromBeat(double beat, double beatsPerCycle) noexcept
    {
        if (beatsPerCycle <= 0.0)
            return 0.0;
        const double cycles = beat / beatsPerCycle;
        return cycles - std::floor(cycles);
    }

    void reset(double startPhase = 0.0) noexcept
    {
        phase = startPhase - std::floor(startPhase);
        held = nextRandom();
    }

    /** Value at the current phase, then advance by `cycles`. */
    float advance(LfoShape shape, double cycles) noexcept LATTICE_NONBLOCKING
    {
        const float v = valueAt(shape, phase);
        setPhase(phase + cycles);
        return v;
    }

    /** Jumps to a phase (synced LFOs); S&H takes a new value whenever a cycle completes. */
    void setPhase(double p) noexcept LATTICE_NONBLOCKING
    {
        const double wrapped = p - std::floor(p);
        if (wrapped < phase - 0.5 || p >= 1.0)
            held = nextRandom();
        phase = wrapped;
    }

    double currentPhase() const noexcept { return phase; }
    void seed(std::uint32_t s) noexcept { random = s == 0 ? 0x1234567u : s; }

    float valueAt(LfoShape shape, double p) const noexcept LATTICE_NONBLOCKING
    {
        const auto x = static_cast<float>(p);
        switch (shape)
        {
        case LfoShape::Sine:
            return std::sin(6.28318530718f * x);
        case LfoShape::Triangle:
            return x < 0.25f ? 4.0f * x : (x < 0.75f ? 2.0f - 4.0f * x : 4.0f * x - 4.0f);
        case LfoShape::SawUp:
            return 2.0f * x - 1.0f;
        case LfoShape::SawDown:
            return 1.0f - 2.0f * x;
        case LfoShape::Square:
            return x < 0.5f ? 1.0f : -1.0f;
        case LfoShape::SampleAndHold:
            return held;
        }
        return 0.0f;
    }

private:
    float nextRandom() noexcept
    {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        return static_cast<float>(random) * 4.6566128730773926e-10f - 1.0f;
    }

    double phase = 0.0;
    float held = 0.0f;
    std::uint32_t random = 0x9e3779b9u;
};

} // namespace lattice::dsp
