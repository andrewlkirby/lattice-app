#pragma once

#include "ui/device/DeviceContext.h"
#include "ui/device/ParameterControls.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace lattice::ui
{

/** What a device shows under its title bar in the Device view (docs/05-m4-ui.md, 4d): its own panel
    (Synth, Sampler, EQ Eight, the racks...) or every parameter ("Show all parameters"). It lays
    itself out for whatever height the Device view has, and says how wide that makes it. */
class DeviceBody : public juce::Component
{
public:
    DeviceBody(DeviceContext& context, juce::ValueTree device);

    const juce::ValueTree& device() const noexcept { return node; }

    /** The width it needs at a height. */
    virtual int preferredWidth(int height) const = 0;
    /** Brings a parameter's control into view (a page of a panel with pages). False if this body
        has no control for it. */
    virtual bool reveal(const juce::String& parameterId);
    /** Applies pending rebuilds now, here and in nested chains (tests). */
    virtual void flushUpdates() {}
    /** The Device view's state for it changed: a rack's selected chain, or map mode. */
    virtual void viewStateChanged() {}
    /** The control for a parameter, or nullptr (also inside nested chains' devices: no). */
    juce::Component* controlFor(const juce::String& parameterId);

protected:
    /** The device type's spec-only instance (specs, groups, ranges). */
    const StudioDevice& prototype() const;
    /** Asks the Device view to lay everything out again (this body's width changed). */
    void widthChanged();

    DeviceContext& context;
    juce::ValueTree node;
};

/** Every parameter group as a titled section, side by side: "Show all parameters", and the body of
    any device without a panel of its own. Every parameter of every device is reachable here. */
class GenericBody final : public DeviceBody
{
public:
    GenericBody(DeviceContext& context, juce::ValueTree device);

    int preferredWidth(int height) const override;
    void resized() override;

    const std::vector<std::unique_ptr<ParameterSection>>& sections() const noexcept { return groups; }

private:
    std::vector<std::unique_ptr<ParameterSection>> groups;
};

/** A row of sections and displays side by side, the layout most panels use: each part takes the
    width it asks for at the body's height. */
class PanelRow : public DeviceBody
{
public:
    using DeviceBody::DeviceBody;

    int preferredWidth(int height) const override;
    void resized() override;

protected:
    /** Adds a part; widthFor gives its width at a height. The row owns it. */
    template <typename T>
    T* addPart(std::unique_ptr<T> part, std::function<int(int height)> widthFor)
    {
        auto* raw = part.get();
        addAndMakeVisible(*raw);
        parts.push_back({std::move(part), std::move(widthFor)});
        return raw;
    }
    /** A section of controls for these parameters (in this order). */
    ParameterSection* addSection(const juce::String& title, const juce::StringArray& parameterIds,
                                 ControlStyle style = ControlStyle::automatic);
    /** A display as wide as `aspect` times the height (clamped to the theme's sizes). */
    template <typename T>
    T* addDisplay(std::unique_ptr<T> display, float aspect)
    {
        return addPart(std::move(display), [this, aspect](int height) { return displayWidth(height, aspect); });
    }
    int displayWidth(int height, float aspect) const;
    int gap() const;

private:
    struct Part
    {
        std::unique_ptr<juce::Component> component;
        std::function<int(int)> widthFor;
    };
    std::vector<Part> parts;
};

/** The body for a device: its own panel, or every parameter when `allParameters` (or when the type
    has no panel of its own). */
std::unique_ptr<DeviceBody> createDeviceBody(DeviceContext& context, const juce::ValueTree& device, bool allParameters);
/** Whether a device type has a panel of its own (so "Show all parameters" means something). */
bool hasOwnPanel(const juce::String& typeId);

} // namespace lattice::ui
