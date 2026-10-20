#pragma once

#include "ui/widgets/BoundControl.h"

namespace lattice::ui
{

/** A rotary knob with its name above and its value below. The arc fills from the start of the
    range, or from the middle for a bipolar value (one whose range is symmetric around 0, such as
    pan or detune). See BoundControl for how it's used. */
class Knob final : public BoundControl
{
public:
    explicit Knob(std::unique_ptr<ValueBinding> binding);

    /** The arc's ends, in radians clockwise from 12 o'clock. */
    static constexpr float startAngle = -2.35619449f; // -135 degrees
    static constexpr float endAngle = 2.35619449f;
    static float angleFor(float normalised) noexcept { return startAngle + normalised * (endAngle - startAngle); }

    void setBipolar(bool shouldBeBipolar);
    bool isBipolar() const noexcept { return bipolar; }
    /** The name shown above the knob (a macro's own name); the parameter's name by default. */
    void setLabel(const juce::String& text);
    const juce::String& label() const noexcept { return labelText; }

    /** The size it's laid out at in the theme (it scales with the UI zoom like everything else). */
    juce::Rectangle<int> preferredSize() const;

    void paint(juce::Graphics&) override;

protected:
    juce::Rectangle<int> typeInArea() const override { return valueArea(); }

private:
    juce::Rectangle<int> labelArea() const;
    juce::Rectangle<int> valueArea() const;
    juce::Rectangle<float> dialArea() const;

    bool bipolar = false;
    juce::String labelText;
};

} // namespace lattice::ui
