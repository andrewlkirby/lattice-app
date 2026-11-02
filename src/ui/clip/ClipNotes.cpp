#include "ui/clip/ClipNotes.h"

#include <algorithm>

namespace lattice::ui
{

ClipNotes::~ClipNotes()
{
    clipTree.removeListener(this);
}

void ClipNotes::setClip(juce::ValueTree c)
{
    if (c != clipTree)
    {
        clipTree.removeListener(this);
        clipTree = std::move(c);
        clipTree.addListener(this);
    }
    notesNode = clipTree.getChildWithName(ids::notes);
    flags.assign(static_cast<std::size_t>(notesNode.getNumChildren()), 0);
    notesChanged();
}

const notes::Notes& ClipNotes::notes() const
{
    if (stale)
    {
        cache.clear();
        cache.reserve(static_cast<std::size_t>(notesNode.getNumChildren()));
        for (const auto& n : notesNode)
            cache.push_back(LiveSet::noteData(n));
        stale = false;
    }
    return cache;
}

notes::Indices ClipNotes::selected() const
{
    notes::Indices out;
    for (std::size_t i = 0; i < flags.size(); ++i)
        if (flags[i] != 0)
            out.push_back(i);
    return out;
}

bool ClipNotes::anySelected() const noexcept
{
    return std::any_of(flags.begin(), flags.end(), [](char f) { return f != 0; });
}

notes::Indices ClipNotes::selectedOrAll() const
{
    return anySelected() ? selected() : notes::all(notes());
}

void ClipNotes::select(const notes::Indices& indices, bool addToSelection)
{
    if (!addToSelection)
        std::fill(flags.begin(), flags.end(), 0);
    for (const auto i : indices)
        if (i < flags.size())
            flags[i] = 1;
    changed();
}

void ClipNotes::deselect(const notes::Indices& indices)
{
    for (const auto i : indices)
        if (i < flags.size())
            flags[i] = 0;
    changed();
}

void ClipNotes::clearSelection()
{
    if (!anySelected())
        return;
    std::fill(flags.begin(), flags.end(), 0);
    changed();
}

void ClipNotes::setSelectionFlags(std::vector<char> newFlags)
{
    newFlags.resize(flags.size(), 0);
    flags = std::move(newFlags);
    changed();
}

void ClipNotes::apply(LiveSet& set, const notes::Change& change)
{
    if (!hasClip() || change.empty())
        return;
    const auto before = flags.size();
    notes::apply(set, clipTree, change);
    setSelectionFlags(notes::selectionAfter(before, change));
}

void ClipNotes::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier&)
{
    if (tree.getParent() == notesNode)
        notesChanged();
    else
        changed(); // the clip's own properties (its loop, its name)
}

void ClipNotes::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child)
{
    if (parent == clipTree && child.hasType(ids::notes))
    {
        setClip(clipTree);
        return;
    }
    if (parent != notesNode)
        return;
    // Notes are appended, except when undo puts a removed one back where it was.
    const int last = parent.getNumChildren() - 1;
    const int index = parent.getChild(last) == child ? last : parent.indexOf(child);
    flags.insert(flags.begin() + index, 0);
    notesChanged();
}

void ClipNotes::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int index)
{
    if (parent != notesNode || index < 0 || static_cast<std::size_t>(index) >= flags.size())
        return;
    flags.erase(flags.begin() + index);
    notesChanged();
}

void ClipNotes::valueTreeChildOrderChanged(juce::ValueTree& parent, int, int)
{
    if (parent != notesNode)
        return;
    std::fill(flags.begin(), flags.end(), 0);
    notesChanged();
}

void ClipNotes::notesChanged()
{
    stale = true;
    ++version;
    changed();
}

void ClipNotes::changed()
{
    if (onChange)
        onChange();
}

} // namespace lattice::ui
