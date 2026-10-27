#include "ui/clip/ClipCommands.h"

namespace lattice::ui
{

void addClipCommands(CommandRegistry& registry, PianoRoll& roll)
{
    using juce::KeyPress;
    using juce::ModifierKeys;
    const auto ctrl = [](int c) { return KeyPress(c, ModifierKeys::commandModifier, 0); };
    const auto hasClip = [&roll] { return roll.hasClip(); };
    const auto hasSelection = [&roll] { return roll.hasClip() && roll.notes().anySelected(); };
    const auto hasNotes = [&roll] { return roll.hasClip() && roll.notes().size() > 0; };
    const auto add = [&registry](const char* id, const char* name, const char* description, juce::Array<KeyPress> keys,
                                 std::function<void()> perform, std::function<bool()> enabled, std::function<bool()> ticked = {}) {
        registry.add({.id = id,
                      .name = name,
                      .category = "Clip",
                      .description = description,
                      .defaultKeys = std::move(keys),
                      .perform = std::move(perform),
                      .isEnabled = std::move(enabled),
                      .isTicked = std::move(ticked)});
    };

    // --- Modes and the grid -----------------------------------------------------------------------
    add("clip.drawMode", "Draw Mode", "In the piano roll, a click adds a note and a click on a note deletes it.", {KeyPress('b')},
        [&roll] { roll.setDrawMode(!roll.isDrawMode()); }, {}, [&roll] { return roll.isDrawMode(); });
    const auto gridCommand = [&](const char* id, const char* name, const char* description, int key, std::function<void(NoteGrid&)> change,
                                 std::function<bool()> ticked = {}) {
        add(id, name, description, {ctrl(key)},
            [&roll, change = std::move(change)] {
                change(roll.grid());
                roll.gridChanged();
            },
            {}, std::move(ticked));
    };
    gridCommand("clip.gridNarrow", "Narrow Grid", "Makes the piano roll's grid finer.", '1', [](NoteGrid& g) { g.narrow(); });
    gridCommand("clip.gridWiden", "Widen Grid", "Makes the piano roll's grid coarser.", '2', [](NoteGrid& g) { g.widen(); });
    gridCommand("clip.gridTriplet", "Triplet Grid", "Switches the piano roll's grid to triplets and back.", '3',
                [](NoteGrid& g) { g.triplet = !g.triplet; }, [&roll] { return roll.grid().triplet; });
    gridCommand("clip.gridSnap", "Snap to Grid", "Notes and markers snap to the grid while you edit them.", '4',
                [](NoteGrid& g) { g.snap = !g.snap; }, [&roll] { return roll.grid().snap; });
    add("clip.gridAdaptive", "Adaptive Grid", "The grid follows the zoom (otherwise it stays at one value).", {ctrl('5')},
        [&roll] { roll.setGridAdaptive(!roll.grid().adaptive); }, {}, [&roll] { return roll.grid().adaptive; });
    add("clip.fold", "Fold", "Shows only the piano roll's rows that have notes.", {}, [&roll] { roll.setFolded(!roll.isFolded()); }, hasClip,
        [&roll] { return roll.isFolded(); });
    add("clip.logRows", "Rows by Frequency", "Spaces the piano roll's rows by the log of their frequencies, so the gaps show the real intervals.", {},
        [&roll] { roll.setRowsLogSpaced(!roll.areRowsLogSpaced()); }, hasClip, [&roll] { return roll.areRowsLogSpaced(); });
    add("clip.follow", "Follow", "The piano roll scrolls to keep the playing position in view.", {KeyPress('f', ModifierKeys::commandModifier | ModifierKeys::shiftModifier, 0)},
        [&roll] { roll.setFollowing(!roll.isFollowing()); }, {}, [&roll] { return roll.isFollowing(); });
    add("clip.velocityLane", "Show Velocities", "Shows or hides the piano roll's velocity lane.", {},
        [&roll] { roll.setVelocityLaneVisible(!roll.isVelocityLaneVisible()); }, {}, [&roll] { return roll.isVelocityLaneVisible(); });
    add("clip.zoomToClip", "Zoom to Clip", "Fits the clip's loop and notes into the piano roll.", {}, [&roll] { roll.zoomToClip(); }, hasClip);
    add("clip.rowsTaller", "Taller Rows", "Makes the piano roll's rows taller.", {}, [&roll] { roll.setRowZoom(roll.rowZoom() * 1.25f); }, hasClip);
    add("clip.rowsShorter", "Shorter Rows", "Makes the piano roll's rows shorter.", {}, [&roll] { roll.setRowZoom(roll.rowZoom() / 1.25f); }, hasClip);

    // --- Note tools -------------------------------------------------------------------------------
    add("clip.split", "Split Notes", "Splits the selected notes at the grid lines inside them.", {ctrl('e')}, [&roll] { roll.split(); }, hasSelection);
    add("clip.join", "Join Notes", "Joins the selected notes of each row into one.", {ctrl('j')}, [&roll] { roll.join(); }, hasSelection);
    add("clip.quantize", "Quantize", "Moves the selected notes (or all of them) towards the grid by the Clip view's amount.", {ctrl('u')},
        [&roll] { roll.quantize(); }, hasNotes);
    add("clip.legato", "Legato", "Each selected note (or each note) lasts until the next one starts.", {}, [&roll] { roll.legato(); }, hasNotes);
    add("clip.reverse", "Reverse Notes", "Mirrors the selected notes (or all of them) in time.", {}, [&roll] { roll.reverse(); }, hasNotes);
    add("clip.invert", "Invert Notes", "Turns the selected notes (or all of them) upside down, within their range.", {}, [&roll] { roll.invert(); }, hasNotes);
    add("clip.stretchDouble", "Stretch Notes x2", "Makes the selected notes (or all of them) twice as long, and twice as far apart.", {},
        [&roll] { roll.stretch(2.0); }, hasNotes);
    add("clip.stretchHalve", "Squeeze Notes /2", "Makes the selected notes (or all of them) half as long, and half as far apart.", {},
        [&roll] { roll.stretch(0.5); }, hasNotes);
    add("clip.transposeUp", "Up a Table Step", "Moves the selected notes (or all of them) up one entry of the track's tuning (Up in the piano roll).", {},
        [&roll] { roll.transposeSteps(1); }, hasNotes);
    add("clip.transposeDown", "Down a Table Step", "Moves the selected notes (or all of them) down one entry of the track's tuning (Down in the piano roll).", {},
        [&roll] { roll.transposeSteps(-1); }, hasNotes);
    add("clip.transposeSemitonesUp", "Up by Semitones", "Moves the selected notes (or all of them) up by the Clip view's semitones, to the nearest entries of the tuning.", {},
        [&roll] { roll.transposeSemitones(roll.transposeAmount); }, hasNotes);
    add("clip.transposeSemitonesDown", "Down by Semitones", "Moves the selected notes (or all of them) down by the Clip view's semitones, to the nearest entries of the tuning.", {},
        [&roll] { roll.transposeSemitones(-roll.transposeAmount); }, hasNotes);
    add("clip.nudgeLeft", "Move Notes Earlier", "Moves the selected notes back by the grid (Left in the piano roll).", {}, [&roll] { roll.nudge(-1); }, hasSelection);
    add("clip.nudgeRight", "Move Notes Later", "Moves the selected notes on by the grid (Right in the piano roll).", {}, [&roll] { roll.nudge(1); }, hasSelection);
    add("clip.shorten", "Shorten Notes", "Shortens the selected notes by the grid (Shift+Left in the piano roll).", {}, [&roll] { roll.resizeBy(-1); }, hasSelection);
    add("clip.lengthen", "Lengthen Notes", "Lengthens the selected notes by the grid (Shift+Right in the piano roll).", {}, [&roll] { roll.resizeBy(1); }, hasSelection);
    add("clip.rampVelocities", "Ramp Velocities", "Sets the selected notes' velocities along a line from the first one's to the last one's.", {},
        [&roll] { roll.rampVelocities(); }, hasNotes);

    // --- The loop ---------------------------------------------------------------------------------
    add("clip.loopDouble", "Double Loop", "Doubles the clip's loop length.", {}, [&roll] { roll.doubleLoop(); }, hasClip);
    add("clip.loopHalve", "Halve Loop", "Halves the clip's loop length.", {}, [&roll] { roll.halveLoop(); }, hasClip);
    add("clip.loopDuplicate", "Duplicate Loop", "Copies the loop's notes after it and doubles its length.", {}, [&roll] { roll.duplicateLoop(); }, hasClip);

    // --- Highlights -------------------------------------------------------------------------------
    add("clip.highlightRows", "Highlight Rows", "Highlights the rows of the selected notes (or turns their highlight off).", {},
        [&roll] { roll.toggleHighlightOfSelectedRows(); }, hasNotes);
    add("clip.highlightForTrack", "Highlight for the Track", "Makes the clip's highlighted rows the track's, for every clip on it without its own.", {},
        [&roll] { roll.moveHighlightToTrack(); }, [&roll] { return roll.hasClip() && roll.clip().hasProperty(ids::highlight); });
    add("clip.clearHighlight", "Clear Highlights", "Removes the clip's and its track's highlighted rows.", {}, [&roll] { roll.clearHighlight(); }, hasClip);
}

} // namespace lattice::ui
