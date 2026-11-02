#include "ui/widgets/Knob.h"

#include <map>
#include <tuple>

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

namespace
{
/** The track arc of a knob this size, made once (every knob of a size has the same). Message
    thread only, as all drawing is. */
const juce::Path& trackArc(float radius, float width, float from, float to)
{
    using Key = std::tuple<float, float, float, float>;
    static std::map<Key, juce::Path> arcs;
    const Key key{radius, width, from, to};
    auto it = arcs.find(key);
    if (it == arcs.end())
    {
        if (arcs.size() >= 256) // the theme's and zoom's sizes: a handful in use
            arcs.clear();
        it = arcs.emplace(key, Knob::arcShape({}, radius, width, from, to)).first;
    }
    return it->second;
}
} // namespace

juce::Path Knob::arcShape(juce::Point<float> centre, float radius, float width, float from, float to)
{
    // Along the outside, round the end, back along the inside, round the start.
    const float half = width * 0.5f;
    const auto start = centre.getPointOnCircumference(radius, from);
    const auto end = centre.getPointOnCircumference(radius, to);
    const auto pi = juce::MathConstants<float>::pi;
    juce::Path p;
    p.addCentredArc(centre.x, centre.y, radius + half, radius + half, 0.0f, from, to, true);
    p.addCentredArc(end.x, end.y, half, half, 0.0f, to, to + pi);
    p.addCentredArc(centre.x, centre.y, radius - half, radius - half, 0.0f, to, from);
    p.addCentredArc(start.x, start.y, half, half, 0.0f, from + pi, from + 2.0f * pi);
    p.closeSubPath();
    return p;
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
        g.setColour(t.knobTrack);
        g.fillPath(trackArc(arcRadius, t.knobArc, startAngle, endAngle), juce::AffineTransform::translation(centre));

        const float from = bipolar ? 0.0f : startAngle;
        if (std::abs(angle - from) > 0.001f)
        {
            g.setColour(!enabled ? t.textDisabled : arcColour.isTransparent() ? t.accent : arcColour);
            g.fillPath(Knob::arcShape(centre, arcRadius, t.knobArc, juce::jmin(from, angle), juce::jmax(from, angle)));
        }

        // The body and its pointer.
        const float bodyRadius = juce::jmax(1.0f, arcRadius - t.knobArc - t.spacing * 0.5f);
        // The border ring as two discs (a stroked ellipse costs several times a filled one).
        const auto body = juce::Rectangle<float>(bodyRadius * 2.0f, bodyRadius * 2.0f).withCentre(centre);
        g.setColour(t.border);
        g.fillEllipse(body.expanded(t.stroke * 0.5f));
        g.setColour(t.surfaceRaised);
        g.fillEllipse(body.reduced(t.stroke * 0.5f));
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
