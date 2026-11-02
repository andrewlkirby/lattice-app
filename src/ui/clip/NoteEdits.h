#pragma once

#include "core/model/LiveSet.h"

#include <cstddef>
#include <vector>

namespace lattice
{
struct TuningTable;
}

namespace lattice::ui::notes
{

/** An edit of a clip's notes, as the piano roll makes it (docs/05-m4-ui.md, 4c): new values for
    some notes, notes to remove, notes to add. Indices are into the clip's notes as they were (the
    children of its NOTES node, in order). Only what changes is touched, so editing one note of
    10,000 is one small undo step. */
struct Change
{
    std::vector<std::pair<std::size_t, NoteData>> changed;
    std::vector<std::size_t> removed;
    std::vector<NoteData> added;

    bool empty() const noexcept { return changed.empty() && removed.empty() && added.empty(); }
};

/** The selected notes afterwards: the changed ones and the added ones (the copies, for a
    duplicate), as flags over the notes after the change. */
std::vector<char> selectionAfter(std::size_t numNotesBefore, const Change& change);
/** The notes after the change (the order apply() leaves them in: added ones at the end). */
std::vector<NoteData> applied(std::vector<NoteData> notes, const Change& change);
/** Makes the change in the clip (no transaction of its own; the caller begins one). */
void apply(LiveSet& set, const juce::ValueTree& clip, const Change& change);

using Notes = std::vector<NoteData>;
using Indices = std::vector<std::size_t>;

/** The shortest note editing makes. */
constexpr double minLength = 1.0 / 128.0;

// --- Moving and sizing ------------------------------------------------------------------------------
/** A move cut short so no note goes before the clip's start or off the 128 rows. */
std::pair<double, int> clampMove(const Notes& notes, const Indices& selected, double quarters, int rows);
/** Moves notes by a time and a number of rows (table steps). The move is cut short so no note goes
    before the clip's start or off the 128 rows, keeping the notes' layout. */
Change move(const Notes& notes, const Indices& selected, double quarters, int rows);
/** Copies of the notes, moved (Alt+drag); the originals stay. */
Change copyMoved(const Notes& notes, const Indices& selected, double quarters, int rows);
/** Changes the notes' ends (dragging the right edge), keeping each at least minLength. */
Change resizeEnd(const Notes& notes, const Indices& selected, double quarters);
/** Moves the notes' starts, keeping their ends (dragging the left edge). */
Change resizeStart(const Notes& notes, const Indices& selected, double quarters);
/** Copies of the notes placed right after them (Ctrl+D): one selection's span later. */
Change duplicate(const Notes& notes, const Indices& selected);
Change remove(const Indices& selected);

// --- Note tools --------------------------------------------------------------------------------------
/** Splits each note at the grid lines inside it (Ctrl+E). */
Change split(const Notes& notes, const Indices& selected, double gridStep);
/** Joins the selected notes of each row into one, from the first's start to the last's end (Ctrl+J).
    The first note's velocity stays. */
Change join(const Notes& notes, const Indices& selected);
/** Moves starts towards the nearest line by amount (0..1); ends too if `ends` (Ctrl+U). */
Change quantize(const Notes& notes, const Indices& selected, double gridStep, double amount, bool ends);
/** Up or down a number of rows: table steps. */
Change transposeSteps(const Notes& notes, const Indices& selected, int steps);
/** Up or down a number of semitones (12-TET intervals): each note goes to the table entry nearest
    its frequency × 2^(semitones/12). noteOffset: the track's (a row's table entry is pitch + it).
    Notes that would leave the table stay. */
Change transposeSemitones(const Notes& notes, const Indices& selected, double semitones, const TuningTable& tuning,
                          int noteOffset);
/** Each note lasts until the next one starts (any row); the last until loopEnd. */
Change legato(const Notes& notes, const Indices& selected, double loopEnd);
/** Mirrors the notes in time within their span. */
Change reverse(const Notes& notes, const Indices& selected);
/** Mirrors the notes' rows within their range (the highest becomes the lowest). */
Change invert(const Notes& notes, const Indices& selected);
/** Stretches the notes in time about the first one's start (×2 or ÷2 in the toolbar). */
Change stretch(const Notes& notes, const Indices& selected, double factor);

// --- Velocity ----------------------------------------------------------------------------------------
Change setVelocity(const Notes& notes, const Indices& selected, int velocity);
/** Adds to every selected note's velocity (dragging a selection's velocities together). */
Change changeVelocity(const Notes& notes, const Indices& selected, int delta);
/** Velocities along a line from (the first note's start, `from`) to (the last's, `to`). */
Change rampVelocity(const Notes& notes, const Indices& selected, int from, int to);

// --- The loop ----------------------------------------------------------------------------------------
/** Copies of the notes in the loop, one loop later (the loop is doubled by the caller). */
Change duplicateLoop(const Notes& notes, double loopStart, double loopEnd);

/** Every note that overlaps the time from..to on rows lowRow..highRow (a marquee). */
Indices inArea(const Notes& notes, double from, double to, int lowRow, int highRow);
/** Every note on one row. */
Indices onRow(const Notes& notes, int row);
/** All of them. */
Indices all(const Notes& notes);

} // namespace lattice::ui::notes
