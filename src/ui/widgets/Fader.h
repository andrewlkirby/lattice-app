#pragma once

#include "ui/widgets/BoundControl.h"
#include "ui/widgets/Meter.h"

namespace lattice::ui
{

/** A vertical fader with a level meter beside it, as on a mixer strip. Dragging moves the cap with
    the pointer (relative, so it never jumps), Shift for fine changes; see BoundControl. For a value
    in dB, a tick marks 0 dB. */
class Fader final : public BoundControl
{
public:
    explicit Fader(std::unique_ptr<ValueBinding> binding);

    Meter& meter() noexcept { return levelMeter; }
    void setShowsMeter(bool shouldShow);

    /** The length of the cap's travel: a drag follows the pointer one to one. */
    float pixelsForFullTravel(bool fine) const override;
    /** The cap's centre for a normalised value, in local coordinates (for tests and the strip's
        dB marks). */
    float capCentreFor(float normalised) const;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::Rectangle<float> trackArea() const;
    float capHeight() const;

    Meter levelMeter{2};
    bool showsMeter = true;
};

} // namespace lattice::ui
