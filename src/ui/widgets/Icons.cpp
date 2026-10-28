#include "ui/widgets/Icons.h"

#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui
{

juce::Path iconPath(Icon icon)
{
    juce::Path p;
    switch (icon)
    {
    case Icon::power:
        p.addCentredArc(0.5f, 0.55f, 0.36f, 0.36f, 0.0f, 0.65f, juce::MathConstants<float>::twoPi - 0.65f, true);
        p.startNewSubPath(0.5f, 0.08f);
        p.lineTo(0.5f, 0.5f);
        break;
    case Icon::foldOpen:
        p.addTriangle(0.15f, 0.3f, 0.85f, 0.3f, 0.5f, 0.75f);
        break;
    case Icon::foldClosed:
        p.addTriangle(0.3f, 0.15f, 0.75f, 0.5f, 0.3f, 0.85f);
        break;
    case Icon::hotSwap:
        p.startNewSubPath(0.1f, 0.35f);
        p.lineTo(0.85f, 0.35f);
        p.startNewSubPath(0.65f, 0.15f);
        p.lineTo(0.85f, 0.35f);
        p.lineTo(0.65f, 0.55f);
        p.startNewSubPath(0.9f, 0.68f);
        p.lineTo(0.15f, 0.68f);
        p.startNewSubPath(0.35f, 0.48f);
        p.lineTo(0.15f, 0.68f);
        p.lineTo(0.35f, 0.88f);
        break;
    case Icon::save:
        p.startNewSubPath(0.5f, 0.08f);
        p.lineTo(0.5f, 0.62f);
        p.startNewSubPath(0.28f, 0.4f);
        p.lineTo(0.5f, 0.62f);
        p.lineTo(0.72f, 0.4f);
        p.startNewSubPath(0.12f, 0.62f);
        p.lineTo(0.12f, 0.9f);
        p.lineTo(0.88f, 0.9f);
        p.lineTo(0.88f, 0.62f);
        break;
    case Icon::allParams:
        for (const float y : {0.22f, 0.5f, 0.78f})
        {
            p.startNewSubPath(0.12f, y);
            p.lineTo(0.88f, y);
        }
        break;
    case Icon::menu:
        p.addTriangle(0.2f, 0.36f, 0.8f, 0.36f, 0.5f, 0.7f);
        break;
    case Icon::up:
        p.addTriangle(0.2f, 0.64f, 0.8f, 0.64f, 0.5f, 0.3f);
        break;
    case Icon::map:
        p.addEllipse(0.08f, 0.08f, 0.38f, 0.38f);
        p.startNewSubPath(0.4f, 0.4f);
        p.lineTo(0.78f, 0.78f);
        p.startNewSubPath(0.55f, 0.85f);
        p.lineTo(0.85f, 0.85f);
        p.lineTo(0.85f, 0.55f);
        break;
    case Icon::plus:
        p.startNewSubPath(0.5f, 0.12f);
        p.lineTo(0.5f, 0.88f);
        p.startNewSubPath(0.12f, 0.5f);
        p.lineTo(0.88f, 0.5f);
        break;
    case Icon::cross:
        p.startNewSubPath(0.18f, 0.18f);
        p.lineTo(0.82f, 0.82f);
        p.startNewSubPath(0.82f, 0.18f);
        p.lineTo(0.18f, 0.82f);
        break;
    }
    return p;
}

bool isStrokedIcon(Icon icon)
{
    return icon != Icon::foldOpen && icon != Icon::foldClosed && icon != Icon::menu && icon != Icon::up;
}

void drawIcon(juce::Graphics& g, Icon icon, juce::Rectangle<float> area, juce::Colour colour, const Theme& theme)
{
    const float size = juce::jmin(area.getWidth(), area.getHeight());
    if (size <= 1.0f)
        return;
    const auto square = area.withSizeKeepingCentre(size, size);
    auto path = iconPath(icon);
    path.applyTransform(juce::AffineTransform::scale(size).translated(square.getX(), square.getY()));
    g.setColour(colour);
    if (isStrokedIcon(icon))
        g.strokePath(path, juce::PathStrokeType(juce::jmax(1.25f, theme.stroke * 1.5f), juce::PathStrokeType::curved,
                                                juce::PathStrokeType::rounded));
    else
        g.fillPath(path);
}

IconButton::IconButton(const juce::String& name, Icon icon, const juce::String& tooltip) : juce::Button(name), shown(icon)
{
    setTooltip(tooltip);
    setTitle(name);
    setDescription(tooltip);
    setWantsKeyboardFocus(false);
    setMouseClickGrabsKeyboardFocus(false);
}

void IconButton::setIcon(Icon i)
{
    if (i != shown)
    {
        shown = i;
        repaint();
    }
}

void IconButton::paintButton(juce::Graphics& g, bool highlighted, bool down)
{
    const auto& t = themeOf(*this);
    const auto r = getLocalBounds().toFloat();
    const bool on = getToggleState();
    if (on || highlighted || down)
    {
        g.setColour(on ? t.accent.withAlpha(down ? 0.6f : 0.35f) : t.surfaceSunken.withAlpha(down ? 1.0f : 0.6f));
        g.fillRoundedRectangle(r.reduced(0.5f), t.radius);
    }
    const auto colour = !isEnabled() ? t.textDisabled : on ? t.text : highlighted ? t.text : t.textDim;
    drawIcon(g, shown, r.reduced(r.getHeight() * 0.22f), colour, t);
}

} // namespace lattice::ui
