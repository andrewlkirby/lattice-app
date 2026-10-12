#pragma once

#include "app/debug/SessionUi.h"
#include "engine/SessionController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace lattice::debugui
{

/** Edits one clip's properties: name, loop, launch mode, quantization, legato and follow actions.
    Shown in a call-out box from the grid's slot menu. Every change is one undo step. */
class ClipPropertiesPanel final : public juce::Component
{
public:
    ClipPropertiesPanel(SessionController& session, juce::ValueTree clip);

    void resized() override;
    void paint(juce::Graphics& g) override;

private:
    void edit(const juce::Identifier& property, const juce::var& value);
    juce::Slider& addBeatsSlider(const juce::Identifier& property, double minimum);
    void addRow(const juce::String& label, juce::Component& control);

    SessionController& session;
    juce::ValueTree clip;

    juce::TextEditor name;
    juce::ComboBox launchMode;
    juce::ComboBox quantization;
    juce::ToggleButton legato{"Legato"};
    juce::ToggleButton looping{"Loop"};
    juce::ComboBox followA;
    juce::ComboBox followB;
    juce::Slider chanceA;
    std::vector<std::unique_ptr<juce::Slider>> beatSliders;
    std::vector<std::pair<std::unique_ptr<juce::Label>, juce::Component*>> rows;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClipPropertiesPanel)
};

} // namespace lattice::debugui
