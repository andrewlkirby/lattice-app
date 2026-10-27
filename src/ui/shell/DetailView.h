#pragma once

#include "ui/clip/ClipView.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lattice::ui
{

/** The detail view along the bottom of the window (docs/05-m4-ui.md, 4a): the Clip view or the
    Device view, one at a time; Shift+Tab switches between them, as in Live. */
class DetailView final : public juce::Component
{
public:
    /** device: what shows for the Device view (a placeholder until 4d). */
    DetailView(ClipView& clipView, juce::Component& device);

    enum class Page
    {
        clip,
        device
    };
    Page page() const noexcept { return shown; }
    void show(Page p);
    void toggle() { show(shown == Page::clip ? Page::device : Page::clip); }

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;

    /** The switch's two halves (local coordinates). */
    juce::Rectangle<int> clipTab() const noexcept { return clipButton; }
    juce::Rectangle<int> deviceTab() const noexcept { return deviceButton; }

private:
    ClipView& clip;
    juce::Component& device;
    Page shown = Page::clip;
    juce::Rectangle<int> clipButton, deviceButton;
};

} // namespace lattice::ui
