#pragma once

#include "core/RealtimeCheck.h"

#include <cstdint>

namespace lattice
{

/** Musical time for the engine (docs/03-m2-engine-session.md, "Transport"). Owned by the audio
    thread; plain value type.

    Positions are in beats (quarter notes). Within each constant-tempo segment the beat at a sample
    is computed from the segment's origin and an integer sample count, never accumulated block by
    block, so nothing drifts: at 120 BPM and 48 kHz, beat 4 is exactly sample 96,000 and beat 7,200
    (one hour) is exactly sample 172,800,000.

    Samples are absolute: the engine's sample counter since prepare(), which keeps running while
    the transport is stopped. */
class Transport
{
public:
    static constexpr double minTempo = 20.0;
    static constexpr double maxTempo = 999.0;

    struct BarPosition
    {
        int bar = 1;       // 1-based
        int beat = 1;      // 1-based, in units of the time signature's denominator
        int sixteenth = 1; // 1-based
    };

    void prepare(double sampleRate) noexcept;

    bool isPlaying() const noexcept { return playing; }
    double tempo() const noexcept { return bpm; }
    double samplesPerBeat() const noexcept { return spb; }
    int timeSigNumerator() const noexcept { return sigNum; }
    int timeSigDenominator() const noexcept { return sigDen; }
    /** Bar length in beats: numerator × 4 / denominator. */
    double beatsPerBar() const noexcept { return sigNum * 4.0 / sigDen; }
    /** Length of one metronome click (one denominator unit) in beats. */
    double beatUnit() const noexcept { return 4.0 / sigDen; }

    /** The beat at an absolute sample while playing; the stopped position while stopped. */
    double beatAt(std::int64_t sample) const noexcept LATTICE_NONBLOCKING;

    /** The first sample at or after a beat (playing only). A tiny tolerance absorbs rounding, so a
        beat that falls exactly on a sample maps to that sample. */
    std::int64_t sampleAtBeat(double beat) const noexcept LATTICE_NONBLOCKING;

    /** Starts playing so that `sample` is at `beat`. */
    void start(std::int64_t sample, double beat) noexcept LATTICE_NONBLOCKING;
    /** Stops and remembers the position at `sample`. */
    void stop(std::int64_t sample) noexcept LATTICE_NONBLOCKING;
    /** While stopped: moves the remembered position (and the bar grid) back to beat 0. */
    void rewind() noexcept LATTICE_NONBLOCKING;
    /** Where Continue resumes (the stopped position). */
    double stoppedBeat() const noexcept { return stoppedAt; }

    /** Changes the tempo from `sample` on (clamped to 20-999 BPM). */
    void setTempo(double newBpm, std::int64_t sample) noexcept LATTICE_NONBLOCKING;

    /** Changes the time signature at `beat`. The bar that contains `beat` (or starts exactly there)
        becomes the first bar of the new signature. */
    void setTimeSignature(int numerator, int denominator, double beat) noexcept LATTICE_NONBLOCKING;

    /** The first grid line at or after `beat` for a grid of gridBeats, counted from the bar grid's
        origin. gridBeats <= 0 returns `beat` unchanged. */
    double nextGridLine(double beat, double gridBeats) const noexcept LATTICE_NONBLOCKING;

    /** Start of the bar that contains `beat`. */
    double barStartAt(double beat) const noexcept LATTICE_NONBLOCKING;

    BarPosition positionOf(double beat) const noexcept LATTICE_NONBLOCKING;

    /** The same for any bar grid (e.g. one published to the UI in a TransportState). */
    static BarPosition positionOf(double beat, double barOrigin, int barOriginIndex, int numerator,
                                  int denominator) noexcept LATTICE_NONBLOCKING;

    /** Beat where the current bar grid (time signature) starts, and that bar's 0-based index. */
    double barGridOrigin() const noexcept { return barOrigin; }
    int barGridOriginIndex() const noexcept { return barOriginIndex; }

private:
    double sampleRate = 48000.0;
    double bpm = 120.0;
    double spb = 24000.0; // samples per beat
    bool playing = false;
    std::int64_t originSample = 0;
    double originBeat = 0.0;
    double stoppedAt = 0.0;

    int sigNum = 4;
    int sigDen = 4;
    double barOrigin = 0.0;
    int barOriginIndex = 0;
};

} // namespace lattice
