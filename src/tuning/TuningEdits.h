#pragma once

#include "tuning/TuningTable.h"

#include <optional>
#include <vector>

namespace lattice::tuning_edits
{

/** What the tuning editor does to a copy of a tuning (docs/05-m4-ui.md, 4f), as plain functions
    on tables. Each returns a new table with the same name, period and page size; labels that are
    the usual ones ("#47 · 440.00 Hz") are made again for the new frequencies, other labels go
    with their entry. Unmapped entries have 0 Hz. */

/** Entry i gets what entry i - offset had: up by `offset` keys. Keys left with nothing are
    unmapped; entries pushed off either end are dropped. */
TuningTable shifted(const TuningTable& table, int offset);

/** Every frequency times `ratio` (> 0). */
TuningTable multiplied(const TuningTable& table, double ratio);

/** A ratio as typed: "3/2", "1.5", or cents ("700c", "-50 cents"). nullopt if it isn't one or
    isn't positive. */
std::optional<double> parseRatio(const juce::String& text);

/** The chosen entries (table indices, in any order; duplicates and unmapped ones are ignored),
    in rising order on consecutive keys from startKey, every other key unmapped. Those that would
    land past key 127 are dropped. */
TuningTable subset(const TuningTable& table, std::vector<int> indices, int startKey);
/** The start key that centres `count` keys on middle C's key (60), within the keyboard. */
int centredStart(int count);
/** The table's anchor notes (TuningTable::isAnchor). */
std::vector<int> anchorIndices(const TuningTable& table);

/** A table's problems before it can be saved or played. */
struct Problem
{
    int row = -1;  // -1: about the whole table
    bool error = false; // errors block saving; the rest are warnings
    juce::String message;
};
struct Check
{
    std::vector<Problem> problems; // by row, whole-table ones first
    int errors = 0;
    int aboveLimit = 0; // mapped entries at or above the playable limit
    bool ok() const noexcept { return errors == 0; }
};
/** Errors: a frequency that isn't positive and finite, one not above the mapped entry before it,
    no entry mapped. Warnings: entries at or above TrackTuning::maxPlayableFraction of the sample
    rate (muted, or folded down, when played); none if sampleRate is 0. */
Check check(const TuningTable& table, double sampleRate);

} // namespace lattice::tuning_edits
