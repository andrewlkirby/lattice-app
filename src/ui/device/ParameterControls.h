#pragma once

#include "core/model/LiveSet.h"
#include "devices/framework/StudioDevice.h"
#include "ui/widgets/Knob.h"
#include "ui/widgets/NumberBox.h"
#include "ui/widgets/Toggle.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace lattice::ui
{

/** How a device parameter shows in a panel. */
enum class ControlStyle
{
    automatic, // a knob for a continuous value, a number box for a choice or a whole number, a toggle for on/off
    knob,
    box,
    toggle
};

/** A number box with its name above it, as a knob shows its name. */
class LabelledBox final : public juce::Component
{
public:
    explicit LabelledBox(std::unique_ptr<ValueBinding> binding);

    NumberBox& box() noexcept { return numberBox; }
    void setLabel(const juce::String& text);
    juce::Rectangle<int> preferredSize() const;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    NumberBox numberBox;
    juce::String label;
};

/** A control for one parameter of a device in the set, bound to the document (one undo step per
    gesture; undo shows in it). */
std::unique_ptr<juce::Component> makeParameterControl(LiveSet& set, const juce::ValueTree& device, const ParameterSpec& spec,
                                                      ControlStyle style = ControlStyle::automatic);

/** The size a control lays out at in its theme (a knob, box, labelled box or toggle; otherwise its
    current size). */
juce::Rectangle<int> preferredSizeOf(const juce::Component& control);
/** The value a control edits, or nullptr if it isn't a bound control. */
ValueBinding* bindingOf(juce::Component& control);
/** The parameter ID a control edits ("" if it edits none). */
juce::String parameterIdOf(juce::Component& control);
/** Every bound control inside `root` (itself included), in child order. */
std::vector<juce::Component*> boundControlsIn(juce::Component& root);

/** Lays controls out in columns inside `area`: top to bottom, then the next column, each column as
    wide as its widest control and the controls `gap` apart. Places them when `place` is true;
    returns the width the columns take either way. */
int flowColumns(const std::vector<juce::Component*>& items, juce::Rectangle<int> area, int gap, bool place);

/** A titled group of controls flowing in columns (a parameter group in "Show all parameters", or a
    block of a custom panel). It owns its controls. */
class ParameterSection final : public juce::Component
{
public:
    explicit ParameterSection(juce::String title = {});

    /** Adds a control it owns; returns it. */
    juce::Component* add(std::unique_ptr<juce::Component> control);
    /** Adds controls for these parameters of a device (IDs it doesn't have are skipped). */
    void addParameters(LiveSet& set, const juce::ValueTree& device, const StudioDevice& prototype,
                       const juce::StringArray& parameterIds, ControlStyle style = ControlStyle::automatic);
    const std::vector<juce::Component*>& controls() const noexcept { return items; }
    const juce::String& title() const noexcept { return heading; }

    /** The width it needs at a height. */
    int preferredWidth(int height) const;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    juce::Rectangle<int> controlArea(juce::Rectangle<int> bounds) const;
    int titleHeight() const;

    juce::String heading;
    std::vector<std::unique_ptr<juce::Component>> owned;
    std::vector<juce::Component*> items;
};

} // namespace lattice::ui
