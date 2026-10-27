#pragma once

#include "engine/SessionController.h"
#include "tuning/TuningRegistry.h"
#include "ui/clip/ClipNotes.h"
#include "ui/clip/NoteGrid.h"
#include "ui/clip/PianoRollLayout.h"
#include "ui/clip/TuningRows.h"
#include "ui/commands/CommandRegistry.h"
#include "ui/commands/EditTarget.h"
#include "ui/feed/EngineStateFeed.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lattice::ui
{

/** The MIDI note editor (docs/05-m4-ui.md, 4c): a ruler of the track's tuning down the left, the
    clip's time across the top (its loop brace and start marker), the notes, and the velocity lane
    under them.

    - Click a note to select it (Shift or Ctrl adds), drag it to move it, its ends to resize it;
      Alt+drag copies. Drag on empty space for a marquee; double-click there to add a note. In draw
      mode (B) a click adds a note (drag for its length) and a click on one deletes it.
    - With the keys: the arrows move the selected notes along the grid or a table step (Shift+Up
      and Down: a page of the tuning, an octave in 12-TET); Shift+Left and Right resize them; the
      Edit commands act on notes; Esc deselects.
    - Clicking a note or a key plays it on the track's instrument, in the track's tuning; clicking
      a key also selects that row's notes. Hovering a key tells its entry, frequency, cents and
      ratio in the info view.
    - The ruler: note names and keys in 12-TET; "index · Hz" in a table tuning (Colundi), with its
      anchors marked, the clip's highlighted entries tinted, and rows the track can't play shaded.
      Rows can be spaced by the log of their frequency, so the gaps show the real intervals.
    - Velocities: drag a stem (a selected one moves every selected note's), draw across stems in
      draw mode, or Shift+drag a line to ramp them.
    - The wheel scrolls (Shift: across); Ctrl+wheel zooms time, Ctrl+Shift+wheel the rows.

    It paints the grid, the notes and the velocities into a cached image, redrawn only after they
    or the view change, and the playhead over it, so playback repaints only the playhead's strip. */
class PianoRoll final : public juce::Component,
                        public EditTarget,
                        private EngineStateFeed::Listener,
                        private juce::ScrollBar::Listener,
                        private juce::ValueTree::Listener,
                        private juce::AsyncUpdater
{
public:
    PianoRoll(SessionController& session, EngineStateFeed& feed, TuningRegistry& tunings);
    ~PianoRoll() override;

    /** Shows a clip (on a track), or nothing (an invalid tree). */
    void setClip(const juce::ValueTree& clip);
    bool hasClip() const noexcept { return clipNotes.hasClip(); }
    const juce::ValueTree& clip() const noexcept { return clipNotes.clip(); }
    /** The track the clip is on (-1: none). */
    int track() const;
    ClipNotes& notes() noexcept { return clipNotes; }
    const PianoRollLayout& layout() const noexcept { return view; }
    NoteGrid& grid() noexcept { return noteGrid; }
    /** The grid's step now (quarter notes), and as it snaps (0 with snapping off). */
    double gridStep() const;
    double snapStep() const { return noteGrid.snap ? gridStep() : 0.0; }
    void gridChanged();
    /** Switching to a fixed grid keeps the value shown now. */
    void setGridAdaptive(bool adaptive);

    // --- Modes and view --------------------------------------------------------------------------
    bool isDrawMode() const noexcept { return drawMode; }
    void setDrawMode(bool on);
    bool isFolded() const noexcept { return folded; }
    void setFolded(bool on);
    bool areRowsLogSpaced() const noexcept { return logRows; }
    void setRowsLogSpaced(bool on);
    /** Row height as a multiple of the theme's (0.5 to 4). */
    void setRowZoom(float zoom);
    float rowZoom() const noexcept { return rowScale; }
    bool isFollowing() const noexcept { return follow; }
    void setFollowing(bool on) { follow = on; }
    bool isVelocityLaneVisible() const noexcept { return showVelocities; }
    void setVelocityLaneVisible(bool visible);
    /** Fits the clip's loop across the view and its notes (or middle C) into it. */
    void zoomToClip();
    void setZoomX(double pixelsPerQuarter, double anchorX);
    void setScroll(double quarters, double y);

    /** Where the clip is playing (quarter notes into it), or -1 when it isn't. */
    double playhead() const { return playheadQuarters(); }
    /** What the ruler tells about a row. */
    TuningRowContext rowContext() const;

    // --- Editing (the commands call these; each is one undo step) -------------------------------------
    void transposeSteps(int steps);
    void transposeSemitones(double semitones);
    void nudge(int direction);    // along the grid (or a 1/16 with no grid)
    void resizeBy(int direction); // the selected notes' ends, by the grid
    void split();
    void join();
    void quantize();
    void legato();
    void reverse();
    void invert();
    void stretch(double factor);
    void doubleLoop();
    void halveLoop();
    void duplicateLoop();
    void rampVelocities();
    void selectAll();
    /** The rows of the selected notes join the clip's highlighted entries (or leave it, if they're
        all in it already). */
    void toggleHighlightOfSelectedRows();
    void toggleHighlight(int pitch);
    void clearHighlight();
    /** Moves the clip's highlight set to its track (every clip on it without its own shows it). */
    void moveHighlightToTrack();

    double quantizeAmount = 1.0; // 0..1
    bool quantizeEnds = false;
    double transposeAmount = 12.0; // semitones, for the transpose buttons

    // EditTarget
    bool canEdit(const juce::String& commandId) const override;
    void edit(const juce::String& commandId) override;

    // --- Areas and hits (local coordinates) -----------------------------------------------------------
    juce::Rectangle<int> rulerArea() const noexcept { return ruler; }
    juce::Rectangle<int> timeArea() const noexcept { return timeline; }
    juce::Rectangle<int> noteArea() const noexcept { return area; }
    juce::Rectangle<int> velocityArea() const noexcept { return velocities; }
    /** A note's rectangle in local coordinates (empty if its row isn't shown). */
    juce::Rectangle<float> noteBounds(std::size_t index) const;
    juce::Point<float> pointFor(double quarters, int pitch) const;

    struct Hit
    {
        enum class Area
        {
            none,
            key,         // the ruler
            time,        // the timeline, outside the markers
            loopStart,   // the loop brace's left end
            loopEnd,     // its right end
            loopBody,    // the brace between them
            startMarker, // the start marker
            note,        // a note's body
            noteStart,   // its left edge
            noteEnd,     // its right edge
            empty,       // the note area, on no note
            velocity     // the velocity lane (note: the stem under the pointer, or -1)
        };
        Area area = Area::none;
        int note = -1;
        int pitch = -1;
        double quarters = 0.0;
    };
    Hit hitAt(juce::Point<float> local) const;

    /** The mouse gestures (the handlers call these; tests too). */
    void press(juce::Point<float> local, juce::ModifierKeys mods);
    void drag(juce::Point<float> local, juce::ModifierKeys mods);
    void release();
    void doubleClick(juce::Point<float> local);

    /** The marquee being dragged (local coordinates; empty if none). */
    juce::Rectangle<float> marquee() const noexcept { return marqueeRect; }

    /** Applies pending changes now (normally the message loop does; tests call it). */
    void flushUpdates() { handleUpdateNowIfNeeded(); }

    /** The info view's text for what's under a point: a key's entry, frequency, cents and ratio. */
    juce::String describeAt(juce::Point<float> local) const;

    void paint(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseExit(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void lookAndFeelChanged() override { contentChanged(); }
    void focusGained(FocusChangeType) override { repaint(); }
    void focusLost(FocusChangeType) override { repaint(); }

    /** Called with the hover text as the pointer moves (the info view shows it). */
    std::function<void(const juce::String&)> onHover;
    /** The commands its right-click menu offers (none without). */
    void setCommands(CommandRegistry* registry) noexcept { commands = registry; }

private:
    void engineStateChanged(std::uint32_t aspects) override;
    void scrollBarMoved(juce::ScrollBar*, double) override;
    void handleAsyncUpdate() override;
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void showMenu(juce::Point<float> local);

    void contentChanged();
    void rowsChanged();
    void updateScrollBars();
    void applyEdit(const juce::String& name, const notes::Change& change);
    void editSelected(const juce::String& name, notes::Change (*make)(const notes::Notes&, const notes::Indices&));
    void audition(int pitch, int velocity);
    void endAudition();
    double playheadQuarters() const;
    juce::Colour noteColour() const;
    void renderContent(juce::Graphics&, juce::Rectangle<int> bounds);
    void paintRuler(juce::Graphics&);
    void paintTimeline(juce::Graphics&);
    void paintPlayhead(juce::Graphics&);
    void repaintPlayhead(double quarters);
    double quartersPerBar() const;
    /** The adaptive grid's closest lines, in pixels. */
    double gridMinPixels() const;
    double quartersAtX(float x) const { return view.quartersAt(static_cast<double>(x - static_cast<float>(area.getX()))); }
    float xAt(double quarters) const { return static_cast<float>(area.getX()) + static_cast<float>(view.xFor(quarters)); }

    SessionController& session;
    EngineStateFeed& feed;
    TuningRegistry& tunings;
    CommandRegistry* commands = nullptr;
    ClipNotes clipNotes;
    juce::ValueTree trackTree;          // the clip's track (its tuning and highlight)
    mutable TuningPtr shownTuning;      // keeps the table rowContext() points into alive
    PianoRollLayout view;
    NoteGrid noteGrid;


    bool drawMode = false, folded = false, logRows = false, follow = true, showVelocities = true;
    float rowScale = 1.0f;
    juce::Rectangle<int> ruler, timeline, area, velocities, corner;
    juce::ScrollBar vertical{true}, horizontal{false};

    // The gesture in progress.
    enum class Gesture
    {
        none,
        moveNotes,
        copyNotes,
        resizeStart,
        resizeEnd,
        drawNote,
        marquee,
        loopStart,
        loopEnd,
        loopBody,
        startMarker,
        velocity,
        velocityDraw,
        velocityRamp,
        key
    };
    Gesture gesture = Gesture::none;
    juce::Point<float> pressPoint;
    Hit pressed;
    notes::Notes pressNotes;  // the notes as they were when the gesture began
    std::vector<char> pressSelection;
    double pressLoopStart = 0.0, pressLoopEnd = 0.0, pressStartMarker = 0.0;
    int lastDragRows = 0;
    juce::Rectangle<float> marqueeRect;
    int auditioning = -1; // the pitch being played (-1: none)
    double insertQuarters = 0.0; // where Paste puts notes

    // Painting.
    juce::Image content;
    bool contentDirty = true;
    double shownPlayhead = -1.0;
};

} // namespace lattice::ui
