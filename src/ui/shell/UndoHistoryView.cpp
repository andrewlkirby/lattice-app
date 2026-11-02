#include "ui/shell/UndoHistoryView.h"

#include "core/model/UndoNames.h"
#include "ui/theme/LatticeLookAndFeel.h"
#include "ui/widgets/Accessible.h"

namespace lattice::ui
{

UndoHistoryView::UndoHistoryView(juce::UndoManager& u) : undo(u)
{
    setTitle("Undo history");
    accessible::markAsView(*this);
    setHelpText("Every change, oldest first. Click one (or use Up and Down) to go back or forward to just after it.");
    setWantsKeyboardFocus(true);
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
    accessible::selectionChanged(*this);
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
            g.setColour(hasKeyboardFocus(false) ? t.selection : t.selection.withAlpha(0.45f));
            g.fillRect(row);
        }
        // What was done, then where, dimmed; the location gives way first when the row is narrow.
        const auto [action, location] = undoNames::split(names[r]);
        auto text = row.reduced(juce::roundToInt(t.padding), 0);
        const auto font = t.font(t.fontNormal);
        const int actionWidth = juce::jmin(text.getWidth(), juce::roundToInt(std::ceil(juce::GlyphArrangement::getStringWidth(font, action))));
        g.setColour(r > at ? t.textDisabled : t.text);
        g.drawText(action, text.removeFromLeft(actionWidth), juce::Justification::centredLeft, true);
        if (location.isNotEmpty() && text.getWidth() > 0)
        {
            g.setColour(r > at ? t.textDisabled.withMultipliedAlpha(0.7f) : t.textDim);
            g.drawText(undoNames::dot() + location, text, juce::Justification::centredLeft, true);
        }
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

bool UndoHistoryView::keyPressed(const juce::KeyPress& key)
{
    if (key.getModifiers().isAnyModifierKeyDown())
        return false;
    const int code = key.getKeyCode();
    if (code == juce::KeyPress::upKey)
        goTo(at - 1);
    else if (code == juce::KeyPress::downKey)
        goTo(at + 1);
    else if (code == juce::KeyPress::homeKey)
        goTo(0);
    else if (code == juce::KeyPress::endKey)
        goTo(names.size() - 1);
    else
        return false;
    return true;
}

juce::String UndoHistoryView::describeCurrent() const
{
    if (at == 0)
        return names.size() > 1 ? "Start, before " + juce::String(names.size() - 1) + " steps" : juce::String("Start, no changes yet");
    return names[at] + ", step " + juce::String(at) + " of " + juce::String(names.size() - 1);
}

std::unique_ptr<juce::AccessibilityHandler> UndoHistoryView::createAccessibilityHandler()
{
    return accessible::viewHandler(*this, juce::AccessibilityRole::list, [this] { return describeCurrent(); });
}

void UndoHistoryView::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    const int visible = juce::jmax(1, (getHeight() - rowHeight()) / juce::jmax(1, rowHeight()));
    scrollRows = juce::jlimit(0, juce::jmax(0, names.size() - visible), scrollRows - (wheel.deltaY > 0 ? 3 : -3));
    repaint();
}

} // namespace lattice::ui
