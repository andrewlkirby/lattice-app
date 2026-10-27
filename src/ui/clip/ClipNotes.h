#pragma once

#include "ui/clip/NoteEdits.h"

#include <juce_data_structures/juce_data_structures.h>

#include <functional>

namespace lattice::ui
{

/** The notes of the clip the piano roll shows, and which of them are selected. It follows the
    clip's NOTES node however it changes (an edit, undo, recording into it), so the selection
    stays with its notes: a note added or removed anywhere moves the flags of the ones after it.

    The note values are read again lazily, all at once, after any of them change, so dragging
    thousands of notes costs one pass per frame, not one per property. Message thread only. */
class ClipNotes final : private juce::ValueTree::Listener
{
public:
    ClipNotes() = default;
    ~ClipNotes() override;

    void setClip(juce::ValueTree clip);
    const juce::ValueTree& clip() const noexcept { return clipTree; }
    bool hasClip() const noexcept { return clipTree.isValid(); }

    const notes::Notes& notes() const;
    std::size_t size() const noexcept { return flags.size(); }

    // --- Selection ---------------------------------------------------------------------------------
    bool isSelected(std::size_t index) const noexcept { return index < flags.size() && flags[index] != 0; }
    notes::Indices selected() const;
    bool anySelected() const noexcept;
    /** The selection, or every note when none is selected (what Live's note tools act on). */
    notes::Indices selectedOrAll() const;
    void select(const notes::Indices& indices, bool addToSelection);
    void deselect(const notes::Indices& indices);
    void clearSelection();
    void setSelectionFlags(std::vector<char> newFlags);
    const std::vector<char>& selectionFlags() const noexcept { return flags; }

    /** Makes an edit to the notes in the model and selects the notes it touched (one undo step for
        the caller's transaction). */
    void apply(LiveSet& set, const notes::Change& change);

    /** Counts every change to the notes (not to the selection): the note layer's cache key. */
    std::uint64_t notesVersion() const noexcept { return version; }

    /** Called after the notes, the selection or the clip's properties change. */
    std::function<void()> onChange;

private:
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int index) override;
    void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override;
    void valueTreeRedirected(juce::ValueTree&) override { setClip(clipTree); }
    void notesChanged();
    void changed();

    juce::ValueTree clipTree, notesNode;
    std::vector<char> flags; // one per note, in the NOTES node's order
    mutable notes::Notes cache;
    mutable bool stale = true;
    std::uint64_t version = 0;
};

} // namespace lattice::ui
