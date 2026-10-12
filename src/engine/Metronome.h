#pragma once

#include "core/RealtimeCheck.h"
#include "engine/Transport.h"

#include <cstdint>

namespace lattice
{

/** A short synthesized click on every beat of the time signature's denominator, with a
    higher-pitched accent on the downbeat (docs/03-m2-engine-session.md, "Metronome").
    Audio thread only. */
class Metronome
{
public:
    static constexpr double accentHz = 1568.0; // G6
    static constexpr double clickHz = 1046.5;  // C6
    static constexpr double clickSeconds = 0.04;

    void prepare(double sampleRate) noexcept;

    /** Adds the clicks that start in [from, to) (absolute samples) and the tail of any click still
        sounding, into out (offset 0 = sample `chunkStart`). Only while the transport plays. */
    void render(const Transport& transport, std::int64_t from, std::int64_t to, std::int64_t chunkStart,
                float* out, float gain) noexcept LATTICE_NONBLOCKING;

    /** Stops a click that is sounding (e.g. when the metronome is turned off). */
    void silence() noexcept { remaining = 0; }

private:
    void renderVoice(float* out, int numSamples, float gain) noexcept LATTICE_NONBLOCKING;

    double sampleRate = 48000.0;
    int clickLength = 1920;
    int remaining = 0;
    double phase = 0.0;
    double increment = 0.0;
    float level = 0.0f;
    float decay = 0.999f;
};

} // namespace lattice
