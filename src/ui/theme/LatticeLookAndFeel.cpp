#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui
{

LatticeLookAndFeel::LatticeLookAndFeel(const Theme& initial) : current(initial)
{
    applyColours();
}

void LatticeLookAndFeel::setTheme(const Theme& t)
{
    current = t;
    applyColours();
}

void LatticeLookAndFeel::applyColours()
{
    const auto& t = current;
    // JUCE's V4 colour scheme, then the individual IDs it doesn't cover.
    // Order: window background, widget background, menu background, outline, default text,
    // default fill, highlighted text, highlighted fill, menu text.
    setColourScheme({t.background, t.surface, t.surfaceRaised, t.border, t.text, t.surfaceRaised, t.accentText,
                     t.accent, t.text});

    setColour(juce::ResizableWindow::backgroundColourId, t.background);
    setColour(juce::DocumentWindow::textColourId, t.text);

    setColour(juce::Label::textColourId, t.text);
    setColour(juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    setColour(juce::Label::outlineColourId, juce::Colours::transparentBlack);
    setColour(juce::Label::textWhenEditingColourId, t.text);
    setColour(juce::Label::backgroundWhenEditingColourId, t.surfaceSunken);
    setColour(juce::Label::outlineWhenEditingColourId, t.focus);

    setColour(juce::TextButton::buttonColourId, t.surfaceRaised);
    setColour(juce::TextButton::buttonOnColourId, t.accent);
    setColour(juce::TextButton::textColourOffId, t.text);
    setColour(juce::TextButton::textColourOnId, t.accentText);

    setColour(juce::ComboBox::backgroundColourId, t.surfaceSunken);
    setColour(juce::ComboBox::textColourId, t.text);
    setColour(juce::ComboBox::outlineColourId, t.border);
    setColour(juce::ComboBox::arrowColourId, t.textDim);
    setColour(juce::ComboBox::focusedOutlineColourId, t.focus);
    setColour(juce::ComboBox::buttonColourId, t.surfaceRaised);

    setColour(juce::PopupMenu::backgroundColourId, t.surfaceRaised);
    setColour(juce::PopupMenu::textColourId, t.text);
    setColour(juce::PopupMenu::headerTextColourId, t.textDim);
    setColour(juce::PopupMenu::highlightedBackgroundColourId, t.accent);
    setColour(juce::PopupMenu::highlightedTextColourId, t.accentText);

    setColour(juce::TextEditor::backgroundColourId, t.surfaceSunken);
    setColour(juce::TextEditor::textColourId, t.text);
    setColour(juce::TextEditor::highlightColourId, t.accent.withAlpha(0.45f));
    setColour(juce::TextEditor::highlightedTextColourId, t.text);
    setColour(juce::TextEditor::outlineColourId, t.border);
    setColour(juce::TextEditor::focusedOutlineColourId, t.focus);
    setColour(juce::TextEditor::shadowColourId, juce::Colours::transparentBlack);
    setColour(juce::CaretComponent::caretColourId, t.text);

    setColour(juce::ScrollBar::thumbColourId, t.knobTrack);
    setColour(juce::ScrollBar::trackColourId, t.surfaceSunken);
    setColour(juce::ScrollBar::backgroundColourId, juce::Colours::transparentBlack);

    setColour(juce::ListBox::backgroundColourId, t.surface);
    setColour(juce::ListBox::textColourId, t.text);
    setColour(juce::ListBox::outlineColourId, t.border);
    setColour(juce::TreeView::backgroundColourId, t.surface);
    setColour(juce::TreeView::linesColourId, t.border);
    setColour(juce::TreeView::selectedItemBackgroundColourId, t.accent.withAlpha(0.35f));

    setColour(juce::AlertWindow::backgroundColourId, t.surfaceRaised);
    setColour(juce::AlertWindow::textColourId, t.text);
    setColour(juce::AlertWindow::outlineColourId, t.border);
    setColour(juce::TooltipWindow::backgroundColourId, t.surfaceRaised);
    setColour(juce::TooltipWindow::textColourId, t.text);
    setColour(juce::TooltipWindow::outlineColourId, t.border);

    setColour(juce::TabbedButtonBar::tabOutlineColourId, t.border);
    setColour(juce::TabbedButtonBar::frontOutlineColourId, t.accent);
    setColour(juce::TabbedComponent::backgroundColourId, t.background);
    setColour(juce::TabbedComponent::outlineColourId, t.border);

    setColour(juce::Slider::thumbColourId, t.accent);
    setColour(juce::Slider::trackColourId, t.knobTrack);
    setColour(juce::Slider::rotarySliderFillColourId, t.accent);
    setColour(juce::Slider::rotarySliderOutlineColourId, t.knobTrack);
    setColour(juce::Slider::textBoxTextColourId, t.text);
    setColour(juce::Slider::textBoxBackgroundColourId, t.surfaceSunken);
    setColour(juce::Slider::textBoxOutlineColourId, t.border);
}

void LatticeLookAndFeel::drawButtonBackground(juce::Graphics& g, juce::Button& button, const juce::Colour& background,
                                              bool highlighted, bool down)
{
    const auto bounds = button.getLocalBounds().toFloat().reduced(0.5f);
    auto fill = background;
    if (!button.isEnabled())
        fill = fill.withMultipliedAlpha(0.5f);
    else if (down)
        fill = fill.darker(0.15f);
    else if (highlighted)
        fill = fill.brighter(0.08f);
    g.setColour(fill);
    g.fillRoundedRectangle(bounds, current.radius);
    g.setColour(button.hasKeyboardFocus(true) ? current.focus : current.border);
    g.drawRoundedRectangle(bounds, current.radius, current.stroke);
}

void LatticeLookAndFeel::drawComboBox(juce::Graphics& g, int width, int height, bool, int, int, int, int,
                                      juce::ComboBox& box)
{
    const auto bounds = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).reduced(0.5f);
    g.setColour(box.findColour(juce::ComboBox::backgroundColourId));
    g.fillRoundedRectangle(bounds, current.radius);
    g.setColour(box.hasKeyboardFocus(true) ? current.focus : current.border);
    g.drawRoundedRectangle(bounds, current.radius, current.stroke);

    // A small chevron on the right.
    const float size = juce::jmin(8.0f, static_cast<float>(height) * 0.35f);
    const float cx = static_cast<float>(width) - current.padding - size * 0.5f, cy = static_cast<float>(height) * 0.5f;
    juce::Path chevron;
    chevron.startNewSubPath(cx - size * 0.5f, cy - size * 0.25f);
    chevron.lineTo(cx, cy + size * 0.25f);
    chevron.lineTo(cx + size * 0.5f, cy - size * 0.25f);
    g.setColour(box.findColour(juce::ComboBox::arrowColourId).withMultipliedAlpha(box.isEnabled() ? 1.0f : 0.5f));
    g.strokePath(chevron, juce::PathStrokeType(1.5f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
}

juce::Font LatticeLookAndFeel::getComboBoxFont(juce::ComboBox&)
{
    return juce::Font(current.font(current.fontNormal));
}

juce::Font LatticeLookAndFeel::getPopupMenuFont()
{
    return juce::Font(current.font(current.fontNormal));
}

juce::Font LatticeLookAndFeel::getTextButtonFont(juce::TextButton&, int buttonHeight)
{
    return juce::Font(current.font(juce::jmin(current.fontNormal, static_cast<float>(buttonHeight) * 0.6f)));
}

void LatticeLookAndFeel::drawTextEditorOutline(juce::Graphics& g, int width, int height, juce::TextEditor& editor)
{
    if (!editor.isEnabled())
        return;
    const auto bounds = juce::Rectangle<float>(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height)).reduced(0.5f);
    g.setColour(editor.hasKeyboardFocus(true) && !editor.isReadOnly() ? current.focus : current.border);
    g.drawRoundedRectangle(bounds, current.radius, current.stroke);
}

const Theme& themeOf(const juce::Component& component)
{
    if (const auto* laf = dynamic_cast<const LatticeLookAndFeel*>(&component.getLookAndFeel()))
        return laf->theme();
    return Theme::dark();
}

} // namespace lattice::ui
