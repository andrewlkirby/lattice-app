#pragma once

#include "ui/widgets/BoundControl.h"

namespace lattice::ui
{

/** A Live-style number box: the value as text in a small well. Drag up or down to change it;
    double-click (or Enter) to type a value; Delete resets it. For a choice parameter, a click opens
    the list of choices. Everything else is as in BoundControl. */
class NumberBox final : public BoundControl
{
public:
    explicit NumberBox(std::unique_ptr<ValueBinding> binding);

    juce::Rectangle<int> preferredSize() const;
    void paint(juce::Graphics&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

    /** For a choice parameter: the menu a click shows (public for tests). */
    juce::PopupMenu choicesMenu();

protected:
    void clicked() override;
};

} // namespace lattice::ui
