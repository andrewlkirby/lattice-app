#include "ui/shell/UndoHistoryView.h"

#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui
{

UndoHistoryView::UndoHistoryView(juce::UndoManager& u) : undo(u)
{
    setTitle("Undo history");
    setHelpText("Every change, oldest first. Click one to go back (or forward) to just after it.");
    undo.addChangeListener(this);
    refresh();
}

UndoHistoryView::~UndoHistoryView()
{
    undo.removeChangeListener(this);
}

int UndoHistoryView::rowHeight() const
{
    return juce::roundToInt(themeOf(*this).fontNormal + themeOf(*this).spacing * 2.0f);
}

void UndoHistoryView::refresh()
{
    const auto done = undo.getUndoDescriptions(); // the latest first
    const auto redo = undo.getRedoDescriptions(); // the next first
    const auto named = [](const juce::String& n) { return n.isNotEmpty() ? n : juce::String("Change"); };
    names.clear();
    names.add("Start");
    for (int i = done.size(); --i >= 0;)
        names.add(named(done[i]));
    at = done.size();
    for (const auto& n : redo)
        names.add(named(n));
    // Keep the current step in view.
    const int visible = juce::jmax(1, (getHeight() - rowHeight()) / juce::jmax(1, rowHeight()));
    if (at < scrollRows || at >= scrollRows + visible)
        scrollRows = juce::jmax(0, at - visible + 1);
    scrollRows = juce::jlimit(0, juce::jmax(0, names.size() - visible), scrollRows);
    repaint();
}

void UndoHistoryView::goTo(int row)
{
    refresh(); // from where the history is now
    row = juce::jlimit(0, names.size() - 1, row);
    // Each step announces a change; the rows are made again once, at the end.
    undo.removeChangeListener(this);
    while (at > row && undo.undo())
        --at;
    while (at < row && undo.redo())
        ++at;
    undo.addChangeListener(this);
    refresh();
}

void UndoHistoryView::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    g.fillAll(t.surface);
    auto area = getLocalBounds();
    const int h = rowHeight();
    auto heading = area.removeFromTop(h);
    g.setColour(t.surfaceRaised);
    g.fillRect(heading);
    g.setColour(t.textDim);
    g.setFont(t.font(t.fontSmall));
    g.drawText("Undo History", heading.reduced(juce::roundToInt(t.padding), 0), juce::Justification::centredLeft, true);
    g.setFont(t.font(t.fontNormal));
    for (int r = scrollRows; r < names.size() && area.getHeight() > 0; ++r)
    {
        const auto row = area.removeFromTop(h);
        if (r == at)
        {
            g.setColour(t.selection.withAlpha(0.45f));
            g.fillRect(row);
        }
        g.setColour(r > at ? t.textDisabled : t.text);
        g.drawText(names[r], row.reduced(juce::roundToInt(t.padding), 0), juce::Justification::centredLeft, true);
    }
}

void UndoHistoryView::resized()
{
    refresh();
}

void UndoHistoryView::mouseDown(const juce::MouseEvent& e)
{
    const int h = rowHeight();
    if (e.y < h)
        return; // the heading
    const int row = scrollRows + (e.y - h) / h;
    if (row < names.size())
        goTo(row);
}

void UndoHistoryView::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    const int visible = juce::jmax(1, (getHeight() - rowHeight()) / juce::jmax(1, rowHeight()));
    scrollRows = juce::jlimit(0, juce::jmax(0, names.size() - visible), scrollRows - (wheel.deltaY > 0 ? 3 : -3));
    repaint();
}

} // namespace lattice::ui
