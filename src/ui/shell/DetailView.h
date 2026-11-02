#pragma once

#include "ui/clip/ClipView.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lattice::ui
{

/** The detail view along the bottom of the window (docs/05-m4-ui.md, 4a): the Clip view, the
    Device view or the Tuning view (4f), one at a time; Shift+Tab switches between the Clip and
    Device views, as in Live. */
class DetailView final : public juce::Component
{
public:
    /** device, tuning: what shows for the Device and Tuning views. */
    DetailView(ClipView& clipView, juce::Component& device, juce::Component& tuning);

    enum class Page
    {
        clip,
        device,
        tuning
    };
    Page page() const noexcept { return shown; }
    void show(Page p);
    void toggle() { show(shown == Page::clip ? Page::device : Page::clip); }
    /** Whose devices the Device view shows ("2 MIDI"), named beside the switch on its page. */
    void setDeviceTitle(const juce::String& title);

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;

    /** The switch's two halves (local coordinates). */
    juce::Rectangle<int> clipTab() const noexcept { return clipButton; }
    juce::Rectangle<int> deviceTab() const noexcept { return deviceButton; }
    juce::Rectangle<int> tuningTab() const noexcept { return tuningButton; }

private:
    ClipView& clip;
    juce::Component& device;
    juce::Component& tuning;
    Page shown = Page::clip;
    juce::String deviceTitle;
    juce::Rectangle<int> clipButton, deviceButton, tuningButton;
};

} // namespace lattice::ui
