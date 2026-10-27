#pragma once

#include "tuning/TrackTuning.h"
#include "tuning/TuningTable.h"

#include <juce_core/juce_core.h>

#include <vector>

namespace lattice::ui
{

/** What the piano roll's ruler shows for one row (docs/05-m4-ui.md, 4c, "Colundi-aware note
    ruler"). The row is a MIDI note; the table entry it plays is that note plus the track's offset.
    - 12-TET: the note's name, with black and white keys.
    - A table tuning (Colundi): "index · Hz", no keys implied; anchors (whole multiples of the base
      frequency) marked; and rows the track can't play (at or above 0.45 × the sample rate, muted
      by the high-note rule) shaded and marked. */
struct TuningRow
{
    int pitch = 0;
    int entry = 0;     // the table entry it plays (pitch + offset), -1 if outside the table
    double hz = 0.0;   // 0: unmapped
    juce::String label;
    bool namedNotes = false; // 12-TET style: black and white keys
    bool blackKey = false;
    bool octaveStart = false; // a C, or (in a table) a page's first entry: a heavier line
    bool anchor = false;
    bool unplayable = false;  // muted (unmapped, outside the table, or too high)
    bool foldsDown = false;   // too high, but the track folds it down a period instead
    bool highlighted = false; // in the clip's (or track's) highlight set
};

struct TuningRowContext
{
    const TuningTable* tuning = nullptr;
    int noteOffset = 0;
    double sampleRate = 48000.0;
    HighNotePolicy policy = HighNotePolicy::Mute;
    std::vector<int> highlight; // table entries, sorted
};

TuningRow tuningRow(const TuningRowContext& context, int pitch);

/** The hover text for a row: its entry, frequency, and its cents and ratio above the table's base
    ("#47 · 440.00 Hz · 4007.8 ct · ×14.667 of 30 Hz"). */
juce::String tuningRowDetails(const TuningRowContext& context, int pitch);

} // namespace lattice::ui
