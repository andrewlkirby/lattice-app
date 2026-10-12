#include "engine/Metronome.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lattice
{

void Metronome::prepare(double newSampleRate) noexcept
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    clickLength = static_cast<int>(clickSeconds * sampleRate);
    // About -60 dB by the end of the click.
    decay = static_cast<float>(std::pow(0.001, 1.0 / clickLength));
    remaining = 0;
}

void Metronome::renderVoice(float* out, int numSamples, float gain) noexcept
{
    const int n = std::min(numSamples, remaining);
    constexpr double twoPi = 2.0 * std::numbers::pi;
    for (int i = 0; i < n; ++i)
    {
        out[i] += gain * level * static_cast<float>(std::sin(twoPi * phase));
        phase += increment;
        if (phase >= 1.0)
            phase -= 1.0;
        level *= decay;
    }
    remaining -= n;
}

void Metronome::render(const Transport& transport, std::int64_t from, std::int64_t to,
                       std::int64_t chunkStart, float* out, float gain) noexcept
{
    if (to <= from)
        return;
    if (!transport.isPlaying())
    {
        renderVoice(out + (from - chunkStart), static_cast<int>(to - from), gain);
        return;
    }

    const double unit = transport.beatUnit();
    const double bar = transport.beatsPerBar();
    // The first click at or after `from`: start one sample early so a click exactly at `from` is
    // found whatever the rounding of beatAt(from).
    double beat = transport.nextGridLine(transport.beatAt(from - 1), unit);
    auto sample = transport.sampleAtBeat(beat);
    while (sample < from)
    {
        beat += unit;
        sample = transport.sampleAtBeat(beat);
    }

    std::int64_t pos = from;
    while (sample < to)
    {
        renderVoice(out + (pos - chunkStart), static_cast<int>(sample - pos), gain);
        pos = sample;

        const double inBar = std::fmod(beat - transport.barGridOrigin(), bar);
        const bool accent = inBar < 1.0e-6 || bar - inBar < 1.0e-6;
        increment = (accent ? accentHz : clickHz) / sampleRate;
        phase = 0.0;
        level = accent ? 1.0f : 0.6f;
        remaining = clickLength;

        beat += unit;
        sample = transport.sampleAtBeat(beat);
    }
    renderVoice(out + (pos - chunkStart), static_cast<int>(to - pos), gain);
}

} // namespace lattice
