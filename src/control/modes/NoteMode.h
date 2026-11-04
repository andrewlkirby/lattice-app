#pragma once

#include "control/modes/NoteLayout.h"
#include "control/modes/StepSequencer.h"
#include "control/surface/Mode.h"
#include "ui/clip/TuningRows.h"

#include <array>
#include <functional>
#include <map>
#include <vector>

namespace lattice::control
{

/** Note mode (docs/06-m5-controllers.md, "Note mode: layouts"): the grid plays the selected MIDI
    track, laid out for its tuning (NoteLayout), at any grid size.

    - Pads play on the fast path (PadPlayer); this mode lights them and shows what's played:
      a pressed pad lights green together with every pad playing the same note.
    - Colours, for a table tuning (Colundi): anchors (whole multiples of the base frequency) in
      the track's colour, the track's highlight set white, the rest dim; notes the track can't
      play (too high, unmapped) unlit, red while pressed. For 12-TET: the root in the track's
      colour, the scale white (Chromatic: the rest dim).
    - Octave up/down (or the up/down arrows) move the grid by a row or a page (table) or an octave;
      with Shift, by one note. Left/right arrows and the track buttons choose the track.
    - Layout cycles the tuning's layouts. Scale opens the Scale/Tuning menu on the pads: a row of
      choices for each setting (tuning, layout, rows, root, scale, octave step), labelled on
      Virtual Push and in the status line; the up/down arrows scroll it.
    - The strip's Scroll setting moves the grid a row for each eighth of the strip.
    The layout is kept on the track's node (not undoable), per tuning: changing the track's tuning
    gives that tuning's default.

    The step sequencers (docs/06-m5-controllers.md, 5d) are Note mode's too:
    - On a track whose chain has a Drum Rack the grid is the Drum layout (SequencerZones::drum):
      its pads (from the rack's first pad on show, shared with the Device view; octave up/down
      move them a page, with Shift a row), the steps of the pad last played, and the loop
      selector. Layout switches to the pads alone (64 on 8x8) and back.
    - Elsewhere Layout cycles the tuning's layouts and then the melodic sequencer: the layout's
      notes up the rows (a table entry, a degree or a semitone a row; octave up/down move them
      an octave, the up/down arrows a row), the steps across, the loop selector below.
    - Both edit the selected slot's clip (or the clip the track plays, if the slot is empty);
      the first step makes a clip in the slot. The step's length is in the Scale menu.
    - While steps are held the encoders and the display are theirs: velocity, length, nudge. */
class NoteMode final : public Mode
{
public:
    explicit NoteMode(ModeHost& host);

    ModeKind kind() const override { return ModeKind::note; }
    bool handle(const SurfaceInput& input) override;
    void render(LedFrame& frame) override;
    void describe(SurfaceText& text) override;
    void padMap(PadNoteMap& map) override;
    std::uint32_t aspects() const override;
    void enter() override;
    void leave() override;
    void selectionChanged() override;
    bool ownsEncoders() const override { return steps.anyHeld() && caps().encoders > 0; }
    void encoderTargets(std::vector<juce::String>& targets) override;
    void display(DisplayFrame& frame) override;

    /** What the pads are now. */
    enum class Pads
    {
        play,      // notes to play (the layout)
        sequencer, // the melodic step sequencer
        drum       // a Drum Rack's layout
    };
    Pads pads() const;
    /** The track's Drum Rack (the first in its chain), or an invalid tree. */
    juce::ValueTree drumRack() const;
    /** The drum's first pad on the grid (the rack's first pad on show). */
    int drumStart() const;
    /** The drum pad the steps are for (its note): the last one played. */
    int drumNote() const;
    /** The note a drum pad plays, or -1 (outside the pads, or past 127). */
    int drumNoteAtPad(int row, int column) const;
    /** Where the sequencer's parts are now (empty for the play layout). */
    SequencerZones zones() const;
    /** The clip the sequencer edits: the selected slot's, or the one the track plays. */
    juce::ValueTree sequencedClip() const;
    StepSequencer::Target sequencerTarget() const;
    StepSequencer& sequencer() noexcept { return steps; }

    /** The track it plays: the selected MIDI track, or -1 (a group, a return, nothing). */
    int track() const;
    NoteLayout layout() const;
    /** Saves a layout for the track (not undoable). */
    void setLayout(const NoteLayout& layout);
    /** The MIDI note a pad plays, or -1. */
    int noteAtPad(int row, int column) const;
    /** What the ruler knows about a note on this track (anchor, highlight, playable). */
    ui::TuningRow rowFor(int note) const;

    bool menuShown() const noexcept { return menu; }
    void setMenuShown(bool shown);

    /** One setting in the Scale/Tuning menu and its choices. */
    struct MenuChoice
    {
        juce::String name;
        bool selected = false;
        std::function<void()> choose;
    };
    struct MenuSetting
    {
        juce::String name;
        std::uint32_t colour = 0;
        std::vector<MenuChoice> choices;
    };
    std::vector<MenuSetting> menuSettings();
    /** Where each setting's choices are on the grid: the setting and choice on each pad (-1, -1
        for none), after the menu's scroll. */
    std::vector<std::pair<int, int>> menuPads(const std::vector<MenuSetting>& settings) const;

private:
    TuningPtr tuningOf(int trackIndex) const;
    ui::TuningRowContext rowContext(int trackIndex) const;
    std::uint32_t trackColour(int trackIndex) const;
    int menuRowsNeeded(const std::vector<MenuSetting>& settings) const;
    void pressMenu(int row, int column);
    void selectTrack(int trackIndex);
    void stepTrack(int direction);
    void moveGrid(int direction);
    void renderMenu(LedFrame& frame);
    void renderDrum(LedFrame& frame, int trackIndex);
    void moveDrum(int direction);
    void moveSequencer(int rows);
    void pressLayout();

    mutable TuningPtr shownTuning; // keeps the table a row context points into alive
    std::array<int, 128> pressedNotes{}; // pads holding each note
    std::map<int, int> padNotes;         // pad index -> the note its press lit
    bool menu = false;
    int menuScroll = 0; // grid rows scrolled
    juce::String touched; // the last menu choice touched, for the status line
    float stripLast = -1.0f;
    StepSequencer steps{host};
    std::map<std::int64_t, int> drumNotes; // track ID -> the drum pad last played
};

} // namespace lattice::control
