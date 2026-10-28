#include "ui/widgets/Knob.h"

#include "ui/theme/LatticeLookAndFeel.h"

#include <cmath>

namespace lattice::ui
{
namespace
{
bool isSymmetricAroundZero(const ParameterSpec& s)
{
    return s.minValue < 0.0f && s.maxValue > 0.0f &&
           std::abs(s.minValue + s.maxValue) <= 1.0e-6f * (s.maxValue - s.minValue);
}

juce::Point<float> onCircle(juce::Point<float> centre, float radius, float angle)
{
    return {centre.x + radius * std::sin(angle), centre.y - radius * std::cos(angle)};
}
} // namespace

Knob::Knob(std::unique_ptr<ValueBinding> b) : BoundControl(std::move(b))
{
    bipolar = isSymmetricAroundZero(binding().spec());
    labelText = binding().spec().name;
    // A knob draws arcs and text: kept as an image, so a view painted again (scrolling, another
    // control moving) copies the knobs that didn't change instead of drawing them again. Its own
    // repaint (a new value, focus, a theme) draws it afresh.
    setBufferedToImage(true);
}

void Knob::setBipolar(bool shouldBeBipolar)
{
    bipolar = shouldBeBipolar;
    repaint();
}

void Knob::setArcColour(juce::Colour c)
{
    arcColour = c;
    repaint();
}

void Knob::setLabel(const juce::String& text)
{
    labelText = text;
    setTitle(text);
    repaint();
}

juce::Rectangle<int> Knob::preferredSize() const
{
    const auto& t = themeOf(*this);
    const float textRow = t.fontSmall + 4.0f;
    const float dial = t.controlHeight * 1.6f;
    return {0, 0, juce::roundToInt(dial + t.padding * 3.0f), juce::roundToInt(dial + 2.0f * textRow + t.spacing)};
}

juce::Rectangle<int> Knob::labelArea() const
{
    const auto& t = themeOf(*this);
    return getLocalBounds().removeFromTop(juce::roundToInt(t.fontSmall + 4.0f));
}

juce::Rectangle<int> Knob::valueArea() const
{
    const auto& t = themeOf(*this);
    return getLocalBounds().removeFromBottom(juce::roundToInt(t.fontSmall + 4.0f));
}

juce::Rectangle<float> Knob::dialArea() const
{
    const auto& t = themeOf(*this);
    auto area = getLocalBounds().toFloat();
    area.removeFromTop(static_cast<float>(labelArea().getHeight()));
    area.removeFromBottom(static_cast<float>(valueArea().getHeight()));
    const float size = juce::jmax(0.0f, juce::jmin(area.getWidth(), area.getHeight()) - t.spacing);
    return area.withSizeKeepingCentre(size, size);
}

void Knob::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    const bool enabled = isEnabled();
    const auto dial = dialArea();
    const auto centre = dial.getCentre();
    const float radius = dial.getWidth() * 0.5f;
    const float arcRadius = radius - t.knobArc * 0.5f;
    const float value = binding().normalised();
    const float angle = angleFor(value);

    if (radius > 2.0f)
    {
        const juce::PathStrokeType arcStroke(t.knobArc, juce::PathStrokeType::curved, juce::PathStrokeType::rounded);
        juce::Path track;
        track.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f, startAngle, endAngle, true);
        g.setColour(t.knobTrack);
        g.strokePath(track, arcStroke);

        const float from = bipolar ? 0.0f : startAngle;
        if (std::abs(angle - from) > 0.001f)
        {
            juce::Path fill;
            fill.addCentredArc(centre.x, centre.y, arcRadius, arcRadius, 0.0f, juce::jmin(from, angle),
                               juce::jmax(from, angle), true);
            g.setColour(!enabled ? t.textDisabled : arcColour.isTransparent() ? t.accent : arcColour);
            g.strokePath(fill, arcStroke);
        }

        // The body and its pointer.
        const float bodyRadius = juce::jmax(1.0f, arcRadius - t.knobArc - t.spacing * 0.5f);
        g.setColour(t.surfaceRaised);
        g.fillEllipse(juce::Rectangle<float>(bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre(centre));
        g.setColour(t.border);
        g.drawEllipse(juce::Rectangle<float>(bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre(centre), t.stroke);
        g.setColour(enabled ? t.text : t.textDisabled);
        g.drawLine(juce::Line<float>(onCircle(centre, bodyRadius * 0.35f, angle), onCircle(centre, bodyRadius, angle)),
                   juce::jmax(1.5f, t.stroke * 2.0f));

        if (hasKeyboardFocus(false))
        {
            g.setColour(t.focus);
            g.drawEllipse(dial.expanded(1.5f), t.stroke);
        }
    }

    g.setFont(t.font(t.fontSmall));
    g.setColour(enabled ? t.textDim : t.textDisabled);
    g.drawFittedText(labelText, labelArea(), juce::Justification::centred, 1, 0.8f);
    if (!isTypingIn())
    {
        g.setColour(!enabled ? t.textDisabled : isDragging() ? t.accent : t.text);
        g.drawFittedText(binding().text(), valueArea(), juce::Justification::centred, 1, 0.8f);
    }
}

} // namespace lattice::ui
