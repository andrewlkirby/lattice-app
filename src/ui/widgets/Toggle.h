#pragma once

#include "ui/theme/Theme.h"
#include "ui/widgets/ControlRegistry.h"
#include "ui/widgets/ValueBinding.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace lattice::ui
{

/** An on/off button lit in its role's colour (docs/05-m4-ui.md, "Toggle buttons"). Bound to an
    on/off value it shows the document's state and each click is one undo step; unbound it toggles
    its own state and calls onClick, like any JUCE button. Space and Enter press it when focused. */
class Toggle final : public juce::Button
{
public:
    enum class Role
    {
        accent,
        play,
        record,
        arm,
        solo,
        mute
    };

    explicit Toggle(const juce::String& caption, Role role = Role::accent);
    /** caption: what it shows (an empty string: the parameter's name). */
    Toggle(std::unique_ptr<ValueBinding> binding, Role role = Role::accent, const juce::String& caption = {});
    ~Toggle() override;

    bool isOn() const noexcept { return getToggleState(); }
    ValueBinding* binding() noexcept { return bound.get(); }
    Role role() const noexcept { return colourRole; }
    juce::Colour onColour(const Theme& theme) const;

    juce::Rectangle<int> preferredSize() const;
    /** A bound toggle registers with the control registry of the window it's in. */
    void parentHierarchyChanged() override;

protected:
    void clicked() override;
    void paintButton(juce::Graphics&, bool highlighted, bool down) override;

private:
    void readBinding();

    std::unique_ptr<ValueBinding> bound;
    Role colourRole;
    ControlRegistry* registry = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(Toggle)
};

/** The more readable of the theme's text and background colours on top of `fill`. */
juce::Colour textOn(juce::Colour fill, const Theme& theme);

} // namespace lattice::ui
