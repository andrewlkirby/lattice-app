#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <cstdint>
#include <vector>

namespace lattice
{
struct TuningTable;
}

namespace lattice::control
{

/** A 12-tone scale: its name and its steps above the root (0 first, ascending, below 12). */
struct Scale
{
    const char* name;
    std::vector<int> steps;
};
/** The scales In Key and Chromatic offer, Major first. */
const std::vector<Scale>& scales();

/** Which note each pad plays in Note mode (docs/06-m5-controllers.md, "Note mode: layouts"), on a
    grid of any size. Pads are counted from the bottom row up, as on Push: the bottom-left pad plays
    `origin`, each pad to the right `columnStep` further, each row up `rowOffset` further.

    - Table (Colundi and any tuning): the steps are table entries.
      index = origin + column x columnStep + row x rowOffset. Presets: Sequential (rowOffset =
      columns: no entry twice) and Overlap 3, 5, 7 (rows share that many entries).
    - In Key (12-TET): the steps are degrees of the scale, so only notes in it are on the grid;
      rows a 4th apart (3 degrees) by default.
    - Chromatic (12-TET): the steps are semitones; rows a 4th apart (5) by default.

    The values are MIDI notes (table entries for a track with no note offset). Plain data:
    a mode keeps one per track, and the track's node saves it (toString / fromString). */
struct NoteLayout
{
    enum class Kind : std::uint8_t
    {
        table,
        inKey,
        chromatic
    };
    /** How far the octave buttons move a table layout: a row, or the whole grid (a page). 12-TET
        layouts always move an octave. */
    enum class OctaveStep : std::uint8_t
    {
        row,
        page
    };

    Kind kind = Kind::table;
    int origin = 0;     // the bottom-left pad's note
    int columnStep = 1; // table only
    int rowOffset = 5;  // entries (table), degrees (In Key) or semitones (Chromatic) per row
    int root = 0;       // 12-TET: 0 = C
    int scale = 0;      // into scales()
    OctaveStep octaveStep = OctaveStep::row;
    /** The pads are the melodic step sequencer (the layout's notes up its rows) instead of notes
        to play. */
    bool sequencer = false;
    /** A Drum Rack's pads fill the grid (64 on 8x8) instead of sharing it with the steps. */
    bool drumPads = false;
    /** The step sequencers' step: an index into stepRates(). */
    int stepRate = defaultStepRate;
    /** The melodic sequencer's bottom row: how many entries, degrees or semitones above origin. */
    int sequencerRow = 0;

    static constexpr int maxRowOffset = 32;
    static constexpr int defaultStepRate = 3; // 1/16

    /** The steps the sequencers offer: (name, length in quarter notes), longest first. */
    static const std::vector<std::pair<juce::String, double>>& stepRates();
    double stepLength() const noexcept;

    /** The note a pad plays (row 0 at the top, as in the grid), or -1 outside 0..127. */
    int noteAt(int row, int column, int rows) const noexcept;
    /** 12-TET: the note's pitch class is the root / in the scale. */
    bool isRoot(int note) const noexcept;
    bool inScale(int note) const noexcept;

    /** How far the octave buttons move origin. */
    int octaveDistance(int rows) const noexcept;
    /** Whether moving by an octave (direction +1 up, -1 down) still leaves a pad playing a note. */
    bool canShift(int direction, int rows, int columns) const noexcept;
    /** Moved by an octave, if it can be. */
    NoteLayout shifted(int direction, int rows, int columns) const noexcept;
    /** Moved by byRows rows (an octave each, In Key): the strip's scroll. Unchanged if no pad would play. */
    NoteLayout scrolled(int byRows, int rows, int columns) const noexcept;
    /** The root changed, the bottom-left keeps its octave. */
    NoteLayout withRoot(int newRoot) const noexcept;

    /** The melodic sequencer: the note of a row of steps (0: the bottom), one entry, degree or
        semitone a row, or -1 outside 0..127. */
    int sequencerNote(int fromBottom) const noexcept;
    /** The sequencer's rows moved by `rows`, if any of `shown` rows would still play a note. */
    NoteLayout sequencerMoved(int rows, int shown) const noexcept;
    /** How far the octave buttons move the sequencer's rows: an octave, or (a table) the rows shown. */
    int sequencerOctave(int shown) const noexcept;

    /** The rows' presets, for this kind and this many columns: (name, rowOffset). */
    static std::vector<std::pair<juce::String, int>> rowPresets(Kind kind, int columns);
    /** The kinds a tuning offers: In Key, Chromatic and Table for one with note names (12-TET),
        Table alone for a table tuning. */
    static std::vector<Kind> kindsFor(const TuningTable& tuning);
    static juce::String kindName(Kind kind);
    /** A tuning's own default: In Key in C for 12-TET (bottom-left C1, MIDI 36); a table starting
        at the computer keyboard's default page, rows offset by 5. */
    static NoteLayout defaultFor(const TuningTable& tuning);
    /** The same layout with another kind (its defaults for that kind, keeping the root). */
    NoteLayout withKind(Kind kind, const TuningTable& tuning) const;

    juce::String toString() const;
    /** Parses toString()'s form; anything missing or invalid keeps `fallback`'s value. */
    static NoteLayout fromString(const juce::String& text, const NoteLayout& fallback);

    bool operator==(const NoteLayout&) const = default;

private:
    /** The lowest and highest notes the grid plays (unclamped). */
    std::pair<int, int> span(int rows, int columns) const noexcept;
};

/** "C", "C#"... */
juce::String pitchClassName(int pitchClass);

} // namespace lattice::control
