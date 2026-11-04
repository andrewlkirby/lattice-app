#pragma once

#include "control/surface/Mode.h"
#include "ui/clip/Steps.h"

#include <vector>

namespace lattice::control
{

/** A rectangle of pads (row 0 at the top). */
struct GridArea
{
    int row = 0, column = 0, rows = 0, columns = 0;

    bool contains(int r, int c) const noexcept { return r >= row && r < row + rows && c >= column && c < column + columns; }
    int count() const noexcept { return rows * columns; }
    bool empty() const noexcept { return count() <= 0; }
    /** A pad's place in the area, row by row from its top-left (-1 outside it). */
    int indexOf(int r, int c) const noexcept { return contains(r, c) ? (r - row) * columns + (c - column) : -1; }
    bool operator==(const GridArea&) const = default;
};

/** Where the step sequencers' parts go on a grid of any size (docs/06-m5-controllers.md, "Drum
    layout" and "Melodic step sequencer").

    - Drum, on a grid at least 6 x 6: the steps along the top half (32 on 8 x 8, 128 on 16 x 16),
      under them the pads on the left (4 x 4 on 8 x 8) and the loop selector on the right. With
      `padsOnly` the pads fill the grid (64 on 8 x 8).
    - Drum, on a smaller grid (4 x 4): the pads fill it, or (not `padsOnly`) the steps fill it but
      for the bottom row, the loop selector.
    - Melodic: the steps above (a row for each note, a column for each step), the loop selector
      along the bottom (a row, or a row for every 8 of them). */
struct SequencerZones
{
    GridArea steps, loop, pads;

    static SequencerZones drum(int rows, int columns, bool padsOnly) noexcept;
    static SequencerZones melodic(int rows, int columns) noexcept;
    /** Whether a grid is big enough for the drum layout's pads, steps and loop at once. */
    static bool fitsAll(int rows, int columns) noexcept { return rows >= 6 && columns >= 6; }
    bool operator==(const SequencerZones&) const = default;
};

/** The step sequencer on a surface: the steps of the clip it edits and the loop selector, for a
    Drum Rack's pad (a row of steps across the whole steps area) or for the notes of a melodic
    layout (a row for each). Note mode runs it; it keeps what's held and the page in view.

    - A step pressed with no note on it gets one at once (a step long, defaultVelocity); one with
      notes loses them when it's let go, unless it was held a while or edited meanwhile. With no
      clip, the first step makes one in the slot, a page long. Each is one undo step.
    - While steps are held the encoders edit their notes (velocity, length, nudge): Note mode
      hands it the encoders and the display.
    - The loop selector shows the clip in pages (a page: what the steps area shows). Pressing a
      page loops it; holding one and pressing another loops from one to the other; with Shift a
      page is only shown. While the clip plays, the page in view follows the playhead (until a
      page is chosen with Shift).
    Message thread. */
class StepSequencer
{
public:
    /** What it edits, worked out by Note mode at each call. */
    struct Target
    {
        int track = -1;
        int scene = -1;           // the slot a clip is made in, if there's none
        juce::ValueTree clip;     // the clip it edits (invalid: none yet)
        double stepLength = 0.25; // quarter notes
        /** The pitch of each row of steps, from the bottom; for a drum, one pitch whose steps run
            row by row along the whole area. */
        std::vector<int> pitches;
        bool oneRow = false;     // the drum's: a single pitch
        std::uint32_t colour = 0;
        /** For a melodic row: whether its note is an anchor or the root (it's tinted) and whether
            it can play. Empty: all plain. */
        std::vector<bool> tinted, playable;
    };

    explicit StepSequencer(ModeHost& host) : host(host) {}

    void setZones(const SequencerZones& zones) { area = zones; }
    const SequencerZones& zones() const noexcept { return area; }

    /** A press in the steps or the loop selector; false for a pad outside them. */
    bool press(const Target& target, int row, int column);
    bool release(const Target& target, int row, int column);
    /** Lets go of everything (leaving the mode, a change of track). */
    void reset();

    void render(const Target& target, LedFrame& frame) const;

    /** Steps per page, and a page's length in quarter notes. */
    int stepsPerPage(const Target& target) const noexcept;
    double pageLength(const Target& target) const noexcept;
    /** The page in view (the playhead's, while following it). */
    int page(const Target& target) const;
    int pages(const Target& target) const;

    /** The steps held now (with their pitch), oldest first. */
    std::vector<ui::steps::Step> heldSteps() const;
    bool anyHeld() const noexcept { return !held.empty(); }
    /** The encoders' addresses while steps are held: velocity, length, nudge. */
    void encoderTargets(const Target& target, std::vector<juce::String>& targets) const;
    void display(const Target& target, DisplayFrame& frame) const;

    /** The step a pad is (with its pitch), if it's one. */
    std::optional<ui::steps::Step> stepAt(const Target& target, int row, int column) const;

    /** How long a step must be held for its notes to stay when it's let go. */
    static constexpr juce::uint32 holdMs = 500;
    /** Tests: the clock held steps are timed by. */
    std::function<juce::uint32()> clock = [] { return juce::Time::getMillisecondCounter(); };

private:
    struct Held
    {
        int pad = -1; // the grid's index
        ui::steps::Step step;
        bool hadNotes = false;
        std::vector<NoteData> before; // its notes when pressed
        juce::uint32 at = 0;
    };
    juce::ValueTree ensureClip(const Target& target);
    int playingStep(const Target& target) const; // -1: the clip isn't playing
    void pressLoop(const Target& target, int page);

    ModeHost& host;
    SequencerZones area;
    std::vector<Held> held;
    std::vector<int> heldPages;
    int viewed = 0;
    bool follow = true;
    std::int64_t viewedClip = 0;
};

} // namespace lattice::control
