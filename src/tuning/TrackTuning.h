#pragma once

#include "core/RealtimeCheck.h"
#include "core/RtShared.h"
#include "tuning/TuningTable.h"

#include <atomic>
#include <cstdint>

namespace lattice
{

enum class BendMode : std::uint8_t
{
    Cents,     // bend by a musical interval (default ±200 cents)
    TableSteps // bend along the table (default ±2 entries), log-interpolated between entries
};

enum class HighNotePolicy : std::uint8_t
{
    Mute,    // notes at or above 0.45 × sample rate don't sound
    FoldDown // divide by the tuning's period until the note is in range
};

/** A track's tuning settings, readable from the audio thread without locks. Every instrument voice
    gets its pitch from hzFor() (docs/02-m1-tuning-core.md). Setters are for the message thread;
    hzFor() is safe on any thread. */
class TrackTuning
{
public:
    /** Fraction of the sample rate at or above which a note is out of range. */
    static constexpr double maxPlayableFraction = 0.45;

    TrackTuning() = default;
    explicit TrackTuning(TuningPtr initial);

    void setTuning(TuningPtr tuning);
    const TuningPtr& getTuning() const noexcept { return table.published(); }

    /** Frees tunings replaced earlier once no reader can still use them. Call regularly. */
    void collectGarbage() { table.collect(); }
    std::size_t pendingGarbage() const noexcept { return table.retiredCount(); }

    void setNoteOffset(int offset) noexcept { noteOffset.store(offset); }
    void setBendMode(BendMode m) noexcept { bendMode.store(m); }
    void setBendRangeCents(float cents) noexcept { bendRangeCents.store(cents); }
    void setBendRangeSteps(float steps) noexcept { bendRangeSteps.store(steps); }
    void setHighNotePolicy(HighNotePolicy p) noexcept { highNotePolicy.store(p); }
    void setSampleRate(double sr) noexcept { sampleRate.store(sr); }

    int getNoteOffset() const noexcept { return noteOffset.load(); }
    BendMode getBendMode() const noexcept { return bendMode.load(); }
    HighNotePolicy getHighNotePolicy() const noexcept { return highNotePolicy.load(); }

    /** Frequency for a MIDI note with a pitch bend in -1..1 (scaled by the bend range of the
        current mode). Applies the note offset, then the bend, then the high-note policy.
        Returns 0 when the note must not sound (unmapped, out of range, or muted). */
    double hzFor(int note, float bend = 0.0f) const noexcept LATTICE_NONBLOCKING;

private:
    RtShared<TuningTable> table;
    std::atomic<int> noteOffset{0};
    std::atomic<BendMode> bendMode{BendMode::Cents};
    std::atomic<float> bendRangeCents{200.0f};
    std::atomic<float> bendRangeSteps{2.0f};
    std::atomic<HighNotePolicy> highNotePolicy{HighNotePolicy::Mute};
    std::atomic<double> sampleRate{0.0}; // 0 = unknown: no high-note policy applied
};

} // namespace lattice
