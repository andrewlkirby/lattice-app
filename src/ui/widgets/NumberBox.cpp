#include "ui/widgets/NumberBox.h"

#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui
{

NumberBox::NumberBox(std::unique_ptr<ValueBinding> b) : BoundControl(std::move(b))
{
}

juce::Rectangle<int> NumberBox::preferredSize() const
{
    const auto& t = themeOf(*this);
    return {0, 0, juce::roundToInt(t.controlHeight * 3.0f), juce::roundToInt(t.controlHeight)};
}

void NumberBox::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    const bool enabled = isEnabled();
    const auto bounds = getLocalBounds().toFloat().reduced(0.5f);
    g.setColour(t.surfaceSunken);
    g.fillRoundedRectangle(bounds, t.radius);
    const auto outline = !enabled ? t.border.withMultipliedAlpha(0.5f)
                         : isDragging() ? t.accent
                         : hasKeyboardFocus(false) ? t.focus
                                                   : t.border;
    g.setColour(outline);
    g.drawRoundedRectangle(bounds, t.radius, t.stroke);

    if (isTypingIn())
        return;
    g.setFont(t.font(t.fontNormal));
    g.setColour(enabled ? t.text : t.textDisabled);
    g.drawFittedText(binding().text(), getLocalBounds().reduced(juce::roundToInt(t.padding * 0.5f), 0),
                     juce::Justification::centred, 1, 0.75f);
}

void NumberBox::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (isEnabled() && !e.mods.isPopupMenu())
        showTypeInEditor();
}

juce::PopupMenu NumberBox::choicesMenu()
{
    juce::PopupMenu menu;
    const auto& s = binding().spec();
    const int current = juce::roundToInt(binding().value());
    juce::Component::SafePointer<NumberBox> safe(this);
    for (int i = 0; i < s.choices.size(); ++i)
        menu.addItem(s.choices[i], true, i == current, [safe, i] {
            if (safe != nullptr)
                safe->binding().setValue(static_cast<float>(i));
        });
    return menu;
}

void NumberBox::clicked()
{
    if (binding().spec().kind == ParamKind::Choice)
        choicesMenu().showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
}

} // namespace lattice::ui
