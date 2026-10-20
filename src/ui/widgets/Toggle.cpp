#include "ui/widgets/Toggle.h"

#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui
{

juce::Colour textOn(juce::Colour fill, const Theme& theme)
{
    return contrastRatio(fill, theme.text) >= contrastRatio(fill, theme.background) ? theme.text : theme.background;
}

Toggle::Toggle(const juce::String& caption, Role role) : juce::Button(caption), colourRole(role)
{
    setClickingTogglesState(true);
    setWantsKeyboardFocus(true);
}

Toggle::Toggle(std::unique_ptr<ValueBinding> b, Role role, const juce::String& caption)
    : juce::Button(caption.isNotEmpty() ? caption : b->spec().name), bound(std::move(b)), colourRole(role)
{
    setClickingTogglesState(false);
    setWantsKeyboardFocus(true);
    setTitle(bound->spec().name);
    bound->onChange = [this] { readBinding(); };
    readBinding();
}

Toggle::~Toggle()
{
    if (bound != nullptr)
        bound->onChange = nullptr;
}

void Toggle::readBinding()
{
    setToggleState(bound->value() > 0.5f, juce::dontSendNotification);
    repaint();
}

void Toggle::clicked()
{
    if (bound != nullptr)
        bound->setValue(bound->value() > 0.5f ? 0.0f : 1.0f); // one undo step; onChange redraws
}

juce::Colour Toggle::onColour(const Theme& t) const
{
    switch (colourRole)
    {
    case Role::accent:
        return t.accent;
    case Role::play:
        return t.play;
    case Role::record:
        return t.record;
    case Role::arm:
        return t.arm;
    case Role::solo:
        return t.solo;
    case Role::mute:
        return t.mute;
    }
    return t.accent;
}

juce::Rectangle<int> Toggle::preferredSize() const
{
    const auto& t = themeOf(*this);
    const float textWidth = juce::GlyphArrangement::getStringWidth(juce::Font(t.font(t.fontNormal)), getButtonText());
    return {0, 0, juce::roundToInt(juce::jmax(t.controlHeight, textWidth + t.padding * 2.0f)), juce::roundToInt(t.controlHeight)};
}

void Toggle::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
    const auto& t = themeOf(*this);
    const bool enabled = isEnabled();
    const bool on = getToggleState();
    const auto bounds = getLocalBounds().toFloat().reduced(0.5f);

    auto fill = on ? onColour(t) : t.surfaceRaised;
    if (!enabled)
        fill = fill.withMultipliedAlpha(0.45f);
    else if (down)
        fill = fill.darker(0.12f);
    else if (highlighted)
        fill = fill.brighter(0.06f);
    g.setColour(fill);
    g.fillRoundedRectangle(bounds, t.radius);
    g.setColour(hasKeyboardFocus(false) ? t.focus : on ? fill.darker(0.25f) : t.border);
    g.drawRoundedRectangle(bounds, t.radius, t.stroke);

    g.setFont(t.font(t.fontNormal));
    g.setColour(!enabled ? t.textDisabled : on ? textOn(fill, t) : t.text);
    g.drawFittedText(getButtonText(), getLocalBounds().reduced(juce::roundToInt(t.padding * 0.5f), 0),
                     juce::Justification::centred, 1, 0.7f);
}

} // namespace lattice::ui
