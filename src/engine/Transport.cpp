#include "engine/Transport.h"

#include <algorithm>
#include <cmath>

namespace lattice
{
namespace
{
// Rounding tolerances. A beat that lands within sampleTolerance of a sample boundary is placed on
// that sample; grid lines within beatTolerance of a beat count as "at" it. Both are far below
// anything audible (0.2 µs at 48 kHz) and far above double rounding error at multi-hour positions.
constexpr double sampleTolerance = 1.0e-5;
constexpr double beatTolerance = 1.0e-9;
} // namespace

void Transport::prepare(double newSampleRate) noexcept
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    spb = 60.0 * sampleRate / bpm;
    playing = false;
    originSample = 0;
    originBeat = 0.0;
    stoppedAt = 0.0;
    barOrigin = 0.0;
    barOriginIndex = 0;
}

double Transport::beatAt(std::int64_t sample) const noexcept
{
    if (!playing)
        return stoppedAt;
    return originBeat + static_cast<double>(sample - originSample) / spb;
}

std::int64_t Transport::sampleAtBeat(double beat) const noexcept
{
    const double offset = (beat - originBeat) * spb;
    return originSample + static_cast<std::int64_t>(std::ceil(offset - sampleTolerance));
}

void Transport::start(std::int64_t sample, double beat) noexcept
{
    playing = true;
    originSample = sample;
    originBeat = beat;
    if (beat == 0.0)
    {
        barOrigin = 0.0;
        barOriginIndex = 0;
    }
}

void Transport::stop(std::int64_t sample) noexcept
{
    if (playing)
        stoppedAt = beatAt(sample);
    playing = false;
}

void Transport::rewind() noexcept
{
    if (playing)
        return;
    stoppedAt = 0.0;
    barOrigin = 0.0;
    barOriginIndex = 0;
}

void Transport::setTempo(double newBpm, std::int64_t sample) noexcept
{
    newBpm = std::clamp(newBpm, minTempo, maxTempo);
    if (newBpm == bpm)
        return;
    if (playing)
    {
        originBeat = beatAt(sample);
        originSample = sample;
    }
    bpm = newBpm;
    spb = 60.0 * sampleRate / bpm;
}

void Transport::setTimeSignature(int numerator, int denominator, double beat) noexcept
{
    if (numerator < 1 || denominator < 1 || (numerator == sigNum && denominator == sigDen))
        return;
    const double oldBar = beatsPerBar();
    const double barIndex = std::floor((beat - barOrigin) / oldBar + beatTolerance);
    barOrigin += std::max(0.0, barIndex) * oldBar;
    barOriginIndex += static_cast<int>(std::max(0.0, barIndex));
    sigNum = numerator;
    sigDen = denominator;
}

double Transport::nextGridLine(double beat, double gridBeats) const noexcept
{
    if (gridBeats <= 0.0)
        return beat;
    const double steps = std::ceil((beat - barOrigin) / gridBeats - beatTolerance);
    return barOrigin + std::max(0.0, steps) * gridBeats;
}

double Transport::barStartAt(double beat) const noexcept
{
    const double bar = beatsPerBar();
    const double index = std::floor((beat - barOrigin) / bar + beatTolerance);
    return barOrigin + std::max(0.0, index) * bar;
}

Transport::BarPosition Transport::positionOf(double beat) const noexcept
{
    return positionOf(beat, barOrigin, barOriginIndex, sigNum, sigDen);
}

Transport::BarPosition Transport::positionOf(double beat, double origin, int originIndex, int numerator,
                                             int denominator) noexcept
{
    numerator = std::max(numerator, 1);
    denominator = std::max(denominator, 1);
    const double bar = numerator * 4.0 / denominator;
    const double rel = std::max(0.0, beat - origin);
    const double barIndex = std::floor(rel / bar + beatTolerance);
    const double within = std::max(0.0, rel - barIndex * bar);
    const double unit = 4.0 / denominator;
    const double beatIndex = std::floor(within / unit + beatTolerance);
    const double sixteenthIndex = std::floor((within - beatIndex * unit) / 0.25 + beatTolerance);

    BarPosition p;
    p.bar = originIndex + static_cast<int>(barIndex) + 1;
    p.beat = static_cast<int>(beatIndex) + 1;
    p.sixteenth = static_cast<int>(sixteenthIndex) + 1;
    return p;
}

} // namespace lattice
