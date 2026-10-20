#include "ui/widgets/Fader.h"

#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui
{

Fader::Fader(std::unique_ptr<ValueBinding> b) : BoundControl(std::move(b))
{
    levelMeter.setInterceptsMouseClicks(true, false);
    addAndMakeVisible(levelMeter);
}

void Fader::setShowsMeter(bool shouldShow)
{
    showsMeter = shouldShow;
    levelMeter.setVisible(shouldShow);
    resized();
    repaint();
}

float Fader::capHeight() const
{
    const auto& t = themeOf(*this);
    return juce::jmax(6.0f, t.controlHeight * 0.45f);
}

juce::Rectangle<float> Fader::trackArea() const
{
    auto area = getLocalBounds().toFloat();
    if (showsMeter)
        area.removeFromRight(static_cast<float>(levelMeter.getWidth()) + themeOf(*this).spacing);
    return area;
}

void Fader::resized()
{
    const auto& t = themeOf(*this);
    const int meterWidth = juce::roundToInt(juce::jmin(static_cast<float>(getWidth()) * 0.4f, t.spacing * 3.0f));
    levelMeter.setBounds(getLocalBounds().removeFromRight(meterWidth).reduced(0, juce::roundToInt(capHeight() * 0.5f)));
}

float Fader::pixelsForFullTravel(bool fine) const
{
    const float travel = juce::jmax(20.0f, trackArea().getHeight() - capHeight());
    return fine ? travel * 10.0f : travel;
}

float Fader::capCentreFor(float normalised) const
{
    const auto track = trackArea();
    const float half = capHeight() * 0.5f;
    return track.getBottom() - half - normalised * juce::jmax(0.0f, track.getHeight() - capHeight());
}

void Fader::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    const bool enabled = isEnabled();
    const auto track = trackArea();
    const float cx = track.getCentreX();
    const float half = capHeight() * 0.5f;
    const float top = track.getY() + half, bottom = track.getBottom() - half;
    const float y = capCentreFor(binding().normalised());

    // The groove, filled up to the cap.
    const float grooveWidth = juce::jmax(3.0f, t.knobArc);
    g.setColour(t.knobTrack);
    g.fillRoundedRectangle(juce::Rectangle<float>(cx - grooveWidth * 0.5f, top, grooveWidth, bottom - top), grooveWidth * 0.5f);
    g.setColour(enabled ? t.accent : t.textDisabled);
    g.fillRoundedRectangle(juce::Rectangle<float>(cx - grooveWidth * 0.5f, y, grooveWidth, bottom - y), grooveWidth * 0.5f);

    // 0 dB.
    const auto& s = binding().spec();
    if (s.unit == ParamUnit::Decibels && s.minValue < 0.0f && s.maxValue > 0.0f)
    {
        const float zero = capCentreFor(binding().toNormalised(0.0f));
        g.setColour(t.textDim);
        g.fillRect(juce::Rectangle<float>(track.getX(), zero - 0.5f, track.getWidth() * 0.2f, 1.0f));
        g.fillRect(juce::Rectangle<float>(track.getRight() - track.getWidth() * 0.2f, zero - 0.5f, track.getWidth() * 0.2f, 1.0f));
    }

    // The cap.
    const auto cap = juce::Rectangle<float>(track.getX() + 1.0f, y - half, track.getWidth() - 2.0f, capHeight());
    g.setColour(t.surfaceRaised);
    g.fillRoundedRectangle(cap, t.radius);
    g.setColour(isDragging() ? t.accent : hasKeyboardFocus(false) ? t.focus : t.border);
    g.drawRoundedRectangle(cap.reduced(0.5f), t.radius, t.stroke);
    g.setColour(enabled ? t.text : t.textDisabled);
    g.fillRect(juce::Rectangle<float>(cap.getX() + cap.getWidth() * 0.2f, y - 0.5f, cap.getWidth() * 0.6f, 1.0f));
}

} // namespace lattice::ui
