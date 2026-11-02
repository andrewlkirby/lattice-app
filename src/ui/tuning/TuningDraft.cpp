#include "ui/tuning/TuningDraft.h"

#include "core/model/UndoNames.h"

#include "tuning/parsers/ColundiTextParser.h"

namespace lattice::ui
{

const juce::Identifier TuningDraft::draftType{"DRAFT"};
const juce::Identifier TuningDraft::entryType{"ENTRY"};
const juce::Identifier TuningDraft::hzId{"hz"};
const juce::Identifier TuningDraft::labelId{"label"};
const juce::Identifier TuningDraft::nameId{"name"};
const juce::Identifier TuningDraft::periodId{"period"};
const juce::Identifier TuningDraft::pageId{"page"};

TuningDraft::TuningDraft() = default;

TuningDraft::~TuningDraft()
{
    tree.removeListener(this);
}

void TuningDraft::begin(const TuningTable& from, const juce::String& draftName, const juce::File& file, const juce::String& basedOn)
{
    tree.removeListener(this);
    undo.clearUndoHistory();
    tree = juce::ValueTree(draftType);
    tree.setProperty(nameId, draftName, nullptr);
    tree.setProperty(periodId, from.periodRatio, nullptr);
    tree.setProperty(pageId, from.keyboardPageSize, nullptr);
    for (int i = 0; i < TuningTable::numNotes; ++i)
    {
        juce::ValueTree entry(entryType);
        const double hz = from.hz[static_cast<std::size_t>(i)];
        const auto& label = from.labels[static_cast<std::size_t>(i)];
        entry.setProperty(hzId, hz, nullptr);
        entry.setProperty(labelId, label == ColundiTextParser::labelFor(i, hz) ? juce::String() : label, nullptr);
        tree.appendChild(entry, nullptr);
    }
    usesNoteNames = from.usesNoteNames;
    target = file;
    base = basedOn;
    open = true;
    tree.addListener(this);
    rebuild();
}

void TuningDraft::end()
{
    tree.removeListener(this);
    undo.clearUndoHistory();
    tree = {};
    current = nullptr;
    open = false;
    target = juce::File();
    base = {};
}

juce::String TuningDraft::name() const
{
    return tree[nameId].toString();
}

void TuningDraft::setName(const juce::String& n)
{
    undo.beginNewTransaction(undoNames::make(undoNames::withValue("Rename Tuning", n.trim()), name()));
    tree.setProperty(nameId, n.trim(), &undo);
}

void TuningDraft::setPeriod(double ratio)
{
    undo.beginNewTransaction(undoNames::make(undoNames::withValue("Tuning Period", juce::String(ratio, 6)), name()));
    tree.setProperty(periodId, juce::jmax(1.000001, ratio), &undo);
}

void TuningDraft::setKeyboardPage(int size)
{
    undo.beginNewTransaction(undoNames::make(undoNames::withValue("Keyboard Page", juce::String(size)), name()));
    tree.setProperty(pageId, juce::jlimit(1, TuningTable::numNotes, size), &undo);
}

void TuningDraft::setHz(int index, double hz)
{
    if (!open || index < 0 || index >= TuningTable::numNotes)
        return;
    undo.beginNewTransaction(undoNames::make(undoNames::withValue("Frequency of " + juce::String(index), juce::String(hz, 3) + " Hz"), name()));
    tree.getChild(index).setProperty(hzId, juce::jmax(0.0, hz), &undo);
}

void TuningDraft::setLabel(int index, const juce::String& label)
{
    if (!open || index < 0 || index >= TuningTable::numNotes)
        return;
    undo.beginNewTransaction(undoNames::make(undoNames::withValue("Label of " + juce::String(index), label.trim()), name()));
    tree.getChild(index).setProperty(labelId, label.trim(), &undo);
}

void TuningDraft::replaceEntries(const TuningTable& edited, const juce::String& stepName)
{
    if (!open)
        return;
    undo.beginNewTransaction(undoNames::make(stepName, name()));
    rebuilding = true; // one table at the end, not 256
    for (int i = 0; i < TuningTable::numNotes; ++i)
    {
        const double hz = edited.hz[static_cast<std::size_t>(i)];
        const auto& label = edited.labels[static_cast<std::size_t>(i)];
        auto entry = tree.getChild(i);
        entry.setProperty(hzId, hz, &undo);
        entry.setProperty(labelId, label == ColundiTextParser::labelFor(i, hz) ? juce::String() : label, &undo);
    }
    rebuilding = false;
    rebuild();
}

void TuningDraft::rebuild()
{
    if (rebuilding || !open)
        return;
    auto t = std::make_shared<TuningTable>();
    t->id = "draft";
    t->name = name();
    t->sourceDescription = target.getFullPathName();
    t->periodRatio = static_cast<double>(tree[periodId]);
    t->keyboardPageSize = static_cast<int>(tree[pageId]);
    t->usesNoteNames = usesNoteNames;
    for (int i = 0; i < TuningTable::numNotes; ++i)
    {
        const auto entry = tree.getChild(i);
        const double hz = static_cast<double>(entry[hzId]);
        const auto label = entry[labelId].toString();
        t->hz[static_cast<std::size_t>(i)] = hz;
        t->labels[static_cast<std::size_t>(i)] = label.isNotEmpty() ? label : ColundiTextParser::labelFor(i, hz);
    }
    current = std::move(t);
    if (onChange)
        onChange();
}

} // namespace lattice::ui
