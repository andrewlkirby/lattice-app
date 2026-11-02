#pragma once

#include "tuning/TuningEdits.h"

#include <juce_data_structures/juce_data_structures.h>

#include <functional>

namespace lattice::ui
{

/** The tuning editor's copy of a tuning (docs/05-m4-ui.md, 4f), being edited before it's saved.

    It's a tree of its own (not in the set): a DRAFT with the name, period and keyboard page, and
    128 ENTRY children with each one's Hz (0: unmapped) and label (empty: the usual one). Edits
    are undone with its own history (undo()), so Ctrl+Z in the Tuning view takes back the last
    edit to the copy, not something in the set; each edit is one step. Message thread only. */
class TuningDraft final : private juce::ValueTree::Listener
{
public:
    TuningDraft();
    ~TuningDraft() override;

    /** Starts a copy of `from`, to be saved as `file`; `basedOn` is the tuning it replaces when
        saved (the ID of the user file being edited, or empty for a new copy). */
    void begin(const TuningTable& from, const juce::String& name, const juce::File& file, const juce::String& basedOn);
    void end();
    bool isOpen() const noexcept { return open; }

    const juce::File& file() const noexcept { return target; }
    void setFile(const juce::File& f) { target = f; }
    const juce::String& basedOn() const noexcept { return base; }

    /** The copy as a table (made again after each change). Its ID is "draft". */
    TuningPtr table() const noexcept { return current; }
    /** Changed since begin() (nothing to lose otherwise): an edit that hasn't been undone. */
    bool isModified() const { return open && undo.canUndo(); }

    juce::String name() const;
    void setName(const juce::String& name);
    void setPeriod(double ratio);
    void setKeyboardPage(int size);
    /** hz 0 unmaps the entry. */
    void setHz(int index, double hz);
    /** An empty label is the usual one. */
    void setLabel(int index, const juce::String& label);
    /** Every entry from `edited` (a shift, multiply or subset), as one step. */
    void replaceEntries(const TuningTable& edited, const juce::String& stepName);

    juce::UndoManager& history() noexcept { return undo; }
    const juce::UndoManager& history() const noexcept { return undo; }
    bool undoLast() { return undo.undo(); }
    bool redoLast() { return undo.redo(); }

    /** The draft's tree (its name, period and page are bound to widgets through it). */
    juce::ValueTree state() const noexcept { return tree; }
    static const juce::Identifier draftType, entryType, hzId, labelId, nameId, periodId, pageId;

    /** After every change (an edit, an undo). */
    std::function<void()> onChange;

private:
    void rebuild();
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override { rebuild(); }
    void valueTreeRedirected(juce::ValueTree&) override { rebuild(); }

    juce::UndoManager undo;
    juce::ValueTree tree;
    TuningPtr current;
    juce::File target;
    juce::String base;
    bool usesNoteNames = false;
    bool open = false;
    bool rebuilding = false;
};

} // namespace lattice::ui
