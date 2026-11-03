#pragma once

#include "control/modes/NoteLayout.h"
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
    gives that tuning's default. */
class NoteMode final : public Mode
{
public:
    explicit NoteMode(ModeHost& host);

    ModeKind kind() const override { return ModeKind::note; }
    bool handle(const SurfaceInput& input) override;
    void render(LedFrame& frame) override;
    void describe(SurfaceText& text) override;
    void padMap(PadNoteMap& map) override;
    std::uint32_t aspects() const override { return ui::EngineStateFeed::transportAspect; } // Play and Record
    void enter() override;
    void leave() override;

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

    mutable TuningPtr shownTuning; // keeps the table a row context points into alive
    std::array<int, 128> pressedNotes{}; // pads holding each note
    std::map<int, int> padNotes;         // pad index -> the note its press lit
    bool menu = false;
    int menuScroll = 0; // grid rows scrolled
    juce::String touched; // the last menu choice touched, for the status line
    float stripLast = -1.0f;
};

} // namespace lattice::control
