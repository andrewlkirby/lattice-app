#pragma once

#include "engine/SessionController.h"
#include "ui/commands/EditTarget.h"
#include "ui/session/SessionSelectionModel.h"
#include "ui/tuning/TrackTuningControls.h"
#include "ui/tuning/TuningDraft.h"
#include "ui/widgets/NumberBox.h"
#include "ui/widgets/Toggle.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace lattice::ui
{

/** The Tuning view (docs/05-m4-ui.md, 4f), a page of the detail view: the selected MIDI track's
    tuning as a table, and the editor for a copy of it.

    Inspector:
    - every one of the 128 entries: index (anchors marked), Hz, cents and ratio from the base,
      label, and whether it plays at the current sample rate with the track's high-note policy
      (muted, or folded down to what);
    - clicking a row plays that entry on the track while the button is held;
    - sorting by any column (click its title), a filter (words matching an index, a frequency or
      a label), anchors only, playable only; Copy (and Ctrl+C) puts the rows shown, or the
      selected ones, on the clipboard as tab-separated text;
    - the track's tuning settings (TrackTuningControls) along the top.

    Editor: Duplicate makes a copy (Edit, for a tuning the editor saved, keeps its file):
    - double-click a Hz or a label (or press Enter) to type it; Delete unmaps the selected rows;
    - Shift by a number of keys, Multiply by a ratio ("3/2", "700c"), Keep Selected or Keep
      Anchors (a subset on consecutive keys around middle C), and the name, period and keyboard
      page;
    - checks as you go: frequencies must rise (errors, which stop saving) and entries too high to
      play are counted (warnings);
    - while the copy is open the track plays it (SnapshotBuilder::setTuningOverride), so clips and
      the keyboard sound the edits; Ctrl+Z here undoes the copy's last edit;
    - Save writes it to the user Tunings folder (ColundiTextParser::write), loads it and gives it
      to the track (and to every track that had the version it replaces), as one undo step;
      Discard drops it. */
class TuningView final : public juce::Component,
                         public EditTarget,
                         private SessionSelectionModel::Listener,
                         private juce::ValueTree::Listener,
                         private TuningRegistry::Listener,
                         private juce::AsyncUpdater
{
public:
    struct Actions
    {
        std::function<double()> sampleRate;
        /** A question (yes empty: a message with OK). */
        std::function<void(const juce::String& title, const juce::String& message, const juce::String& yes, std::function<void()>)> confirm;
        /** A question with a text answer. */
        std::function<void(const juce::String& title, const juce::String& message, const juce::String& initial,
                           std::function<void(const juce::String&)>)>
            ask;
        /** "Load Tuning File..." for a track. */
        std::function<void(int track)> loadFile;
        juce::File userTunings; // where copies are saved
    };

    TuningView(SessionController& session, TuningRegistry& tunings, SessionSelectionModel& selection, Actions actions);
    ~TuningView() override;

    enum class Column
    {
        index,
        hz,
        cents,
        ratio,
        label,
        playable
    };
    static constexpr int numColumns = 6;
    static juce::String columnName(Column c);

    /** The track shown (-1: none, e.g. a group is selected); while a copy is open, its track. */
    int track() const;
    /** What the table shows: the copy while editing, else the track's tuning. */
    TuningPtr shown() const;

    // --- Inspecting ------------------------------------------------------------------------------
    /** The entries shown, in the order shown. */
    const std::vector<int>& rows() const noexcept { return visible; }
    void sortBy(Column column, bool ascending);
    Column sortColumn() const noexcept { return sortedBy; }
    bool sortAscending() const noexcept { return ascending; }
    void setFilter(const juce::String& text);
    void setAnchorsOnly(bool on);
    void setPlayableOnly(bool on);
    /** A cell's text, as shown and copied. */
    juce::String cellText(int entry, Column column) const;
    /** The rows shown (or the selected ones), with a header line, tab-separated. */
    juce::String tableText(bool selectedOnly) const;
    void copyToClipboard();

    std::vector<int> selectedEntries() const;
    void selectEntries(const std::vector<int>& entries);
    /** Plays an entry on the track (velocity 0 ends it). */
    void playEntry(int entry, int velocity);

    // --- Editing ---------------------------------------------------------------------------------
    bool isEditing() const noexcept { return draft.isOpen(); }
    TuningDraft& editDraft() noexcept { return draft; }
    /** A copy of the shown tuning, named "<name> copy" (a free name in the user folder). */
    void duplicate();
    /** Whether the shown tuning is a file in the user Tunings folder that Edit reopens. */
    bool canEditInPlace() const;
    void editInPlace();
    /** Saves the copy (asks first if that replaces another file). False if it couldn't, or asked. */
    bool save();
    /** Ends the copy (asks first if it was changed, unless `force`). */
    void discard(bool force = false);
    /** The table's checks for the copy, at the current sample rate. */
    tuning_edits::Check problems() const;

    /** A typed cell: a Hz ("440", "1.2k", "-" unmaps) or a label. False if it isn't valid. */
    bool setCell(int entry, Column column, const juce::String& text);
    /** Opens the in-place editor on a cell of the copy. */
    void editCell(int entry, Column column);
    void shiftBy(int keys);
    /** False if the text isn't a ratio. */
    bool multiplyBy(const juce::String& ratio);
    void keepSelected();
    void keepAnchors();
    void unmapSelected();

    /** A different set was loaded or made: an open copy goes (track IDs start again in every
        set, so its track's ID may now be another track's). */
    void setReplaced();

    /** Applies pending changes now (tests). */
    void flushUpdates() { handleUpdateNowIfNeeded(); }

    // EditTarget: Copy and Select All, and (editing) Delete, Undo and Redo.
    bool canEdit(const juce::String& commandId) const override;
    void edit(const juce::String& commandId) override;
    /** This view, if it has the keys. */
    EditTarget* editTargetIfFocused();

    void paint(juce::Graphics&) override;
    void resized() override;
    void lookAndFeelChanged() override;

    juce::TextButton& duplicateButton() noexcept { return duplicateBtn; }
    juce::TextButton& saveButton() noexcept { return saveBtn; }
    TrackTuningControls* trackControls() noexcept { return controls.get(); }
    juce::Component& tableComponent();

private:
    class Table;
    class Header;

    void selectionChanged(const SessionSelection& now) override;
    void tuningsChanged() override { triggerAsyncUpdate(); }
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { triggerAsyncUpdate(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override;

    void refresh();
    void rebuildRows();
    void updateButtons();
    void draftChanged();
    void beginDraft(const TuningTable& from, const juce::String& name, const juce::File& file, const juce::String& basedOn);
    bool writeAndUse(const juce::File& file);
    int indexOfTrack(std::int64_t id) const;
    /** The track's note offset: key k plays entry k + offset. */
    int noteOffset() const;
    double sampleRate() const;
    double playableLimit() const;
    juce::String uniqueName(const juce::String& wanted) const;

    SessionController& session;
    TuningRegistry& registry;
    SessionSelectionModel& selection;
    Actions actions;
    TuningDraft draft;

    std::int64_t shownTrack = 0; // its ID; while editing, the copy's track
    std::vector<int> visible;
    std::vector<bool> chosen = std::vector<bool>(TuningTable::numNotes, false);
    Column sortedBy = Column::index;
    bool ascending = true;
    juce::StringArray filterWords;
    bool anchorsOnly = false, playableOnly = false;
    int playing = -1; // the key sounding (-1: none)

    std::unique_ptr<TrackTuningControls> controls;
    juce::TextButton duplicateBtn{"Duplicate"}, editBtn{"Edit"}, loadBtn{"Load File..."}, saveBtn{"Save"}, discardBtn{"Discard"};
    juce::TextButton shiftBtn{"Shift..."}, multiplyBtn{"Multiply..."}, keepSelectedBtn{"Keep Selected"},
        keepAnchorsBtn{"Keep Anchors"}, copyBtn{"Copy"};
    juce::TextEditor filterBox, nameBox;
    Toggle anchorsToggle{"Anchors"}, playableToggle{"Playable"};
    std::unique_ptr<NumberBox> periodBox, pageBox;
    std::unique_ptr<Header> header;
    std::unique_ptr<Table> table;
    juce::Viewport viewport;
    juce::Rectangle<int> titleArea, statusArea;
};

} // namespace lattice::ui
