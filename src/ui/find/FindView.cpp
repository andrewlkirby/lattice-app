#include "ui/find/FindView.h"

#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui
{

FindView::FindView(FindIndex& i) : index(i)
{
    setTitle("Find");
    field.setTitle("Find");
    field.addListener(this);
    field.addKeyListener(this);
    addAndMakeVisible(field);
}

FindView::~FindView()
{
    field.removeKeyListener(this);
    field.removeListener(this);
}

void FindView::open(bool palette)
{
    commandsOnly = palette;
    const auto& t = themeOf(*this);
    field.setTextToShowWhenEmpty(palette ? "Run a command..." : "Find a control, a setting or a command", t.textDim);
    field.applyColourToAllText(t.text);
    field.setText({}, false);
    refresh();
    setVisible(true);
    toFront(false);
    if (field.isShowing())
        field.grabKeyboardFocus();
}

void FindView::close()
{
    setVisible(false);
    if (onClose)
        onClose();
}

void FindView::setText(const juce::String& text)
{
    field.setText(text, false);
    refresh();
}

void FindView::refresh()
{
    shown = index.search(field.getText(), selectedTrack ? selectedTrack() : -1, commandsOnly, maxResults);
    selected = 0;
    firstShown = 0;
    if (auto* parent = getParentComponent())
        setBounds(getBounds().withHeight(juce::jmin(preferredHeight(), parent->getHeight() - getY())));
    repaint();
}

void FindView::moveSelection(int delta)
{
    if (shown.empty())
        return;
    selected = juce::jlimit(0, static_cast<int>(shown.size()) - 1, selected + delta);
    const int rows = visibleRows();
    if (selected < firstShown)
        firstShown = selected;
    else if (selected >= firstShown + rows)
        firstShown = selected - rows + 1;
    repaint();
}

void FindView::chooseSelected()
{
    if (selected < 0 || selected >= static_cast<int>(shown.size()))
        return;
    const auto result = shown[static_cast<std::size_t>(selected)];
    close();
    if (onChoose)
        onChoose(result);
}

bool FindView::keyPressed(const juce::KeyPress& key, juce::Component*)
{
    if (key == juce::KeyPress::upKey)
        moveSelection(-1);
    else if (key == juce::KeyPress::downKey)
        moveSelection(1);
    else if (key == juce::KeyPress::pageUpKey)
        moveSelection(-visibleRows());
    else if (key == juce::KeyPress::pageDownKey)
        moveSelection(visibleRows());
    else
        return false;
    return true;
}

int FindView::rowHeight() const
{
    const auto& t = themeOf(*this);
    return juce::roundToInt(t.fontNormal + t.fontSmall + t.spacing * 3.0f);
}

int FindView::visibleRows() const
{
    const auto& t = themeOf(*this);
    const int fieldHeight = juce::roundToInt(t.controlHeight * 1.3f) + juce::roundToInt(t.padding * 2.0f);
    return juce::jmax(1, (getHeight() - fieldHeight) / rowHeight());
}

int FindView::preferredHeight() const
{
    const auto& t = themeOf(*this);
    const int fieldHeight = juce::roundToInt(t.controlHeight * 1.3f) + juce::roundToInt(t.padding * 2.0f);
    const int rows = juce::jmin(12, juce::jmax(static_cast<int>(shown.size()), field.isEmpty() ? 0 : 1));
    return fieldHeight + rows * rowHeight() + juce::roundToInt(t.padding);
}

juce::Rectangle<int> FindView::rowArea(int visibleIndex) const
{
    const auto& t = themeOf(*this);
    const int top = juce::roundToInt(t.controlHeight * 1.3f) + juce::roundToInt(t.padding * 2.0f);
    return {0, top + visibleIndex * rowHeight(), getWidth(), rowHeight()};
}

void FindView::resized()
{
    const auto& t = themeOf(*this);
    field.setBounds(getLocalBounds().reduced(juce::roundToInt(t.padding)).withHeight(juce::roundToInt(t.controlHeight * 1.3f)));
    field.applyFontToAllText(t.font(t.fontLarge));
}

void FindView::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    g.setColour(t.surfaceRaised);
    g.fillRoundedRectangle(getLocalBounds().toFloat(), t.radius);
    g.setColour(t.border);
    g.drawRoundedRectangle(getLocalBounds().toFloat().reduced(0.5f), t.radius, 1.0f);
    if (shown.empty() && !field.isEmpty())
    {
        g.setColour(t.textDim);
        g.setFont(t.font(t.fontNormal));
        g.drawText("Nothing found.", rowArea(0).reduced(juce::roundToInt(t.padding * 1.5f), 0), juce::Justification::centredLeft, true);
        return;
    }
    const int rows = visibleRows();
    for (int v = 0; v < rows && firstShown + v < static_cast<int>(shown.size()); ++v)
    {
        const int i = firstShown + v;
        const auto& r = shown[static_cast<std::size_t>(i)];
        auto area = rowArea(v);
        if (i == selected)
        {
            g.setColour(t.selection.withAlpha(0.45f));
            g.fillRect(area);
        }
        area = area.reduced(juce::roundToInt(t.padding * 1.5f), juce::roundToInt(t.spacing));
        g.setColour(t.text);
        g.setFont(t.font(t.fontNormal));
        g.drawText(r.name, area.removeFromTop(juce::roundToInt(t.fontNormal + t.spacing)), juce::Justification::centredLeft, true);
        g.setColour(t.textDim);
        g.setFont(t.font(t.fontSmall));
        g.drawText(r.location, area, juce::Justification::centredLeft, true);
    }
}

void FindView::mouseDown(const juce::MouseEvent& e)
{
    for (int v = 0; v < visibleRows(); ++v)
        if (rowArea(v).contains(e.getPosition()) && firstShown + v < static_cast<int>(shown.size()))
        {
            selected = firstShown + v;
            chooseSelected();
            return;
        }
}

void FindView::mouseMove(const juce::MouseEvent& e)
{
    for (int v = 0; v < visibleRows(); ++v)
        if (rowArea(v).contains(e.getPosition()) && firstShown + v < static_cast<int>(shown.size()) && selected != firstShown + v)
        {
            selected = firstShown + v;
            repaint();
        }
}

void FindView::mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    firstShown = juce::jlimit(0, juce::jmax(0, static_cast<int>(shown.size()) - visibleRows()), firstShown + (wheel.deltaY > 0 ? -3 : 3));
    repaint();
}

} // namespace lattice::ui
