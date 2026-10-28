#pragma once

// Shared bits of the M3 generic device editor (docs/04-m3-devices.md, "3f"). Throwaway like the
// rest of src/app/debug/: M4's device views replace the visuals and keep the same data.

#include "app/debug/SessionUi.h"
#include "core/model/LiveSet.h"
#include "devices/framework/DeviceParameter.h"
#include "devices/framework/DeviceRegistry.h"
#include "devices/framework/DeviceSpecs.h"
#include "devices/framework/StudioDevice.h"
#include "engine/SessionController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <memory>

namespace lattice::debugui
{

/** What is being dragged, as a juce::var drag description:
    - "device:<typeId>"   a new device from the device list
    - "preset:<index>"    a preset from the library (index into PresetLibrary::all())
    - "sample:<path>"     a sample file (also files dragged in from the OS)
    - "move:<deviceId>"   a device already in the set */
namespace drag
{
inline juce::String device(const juce::String& typeId) { return "device:" + typeId; }
inline juce::String preset(int index) { return "preset:" + juce::String(index); }
inline juce::String sample(const juce::File& f) { return "sample:" + f.getFullPathName(); }
inline juce::String move(std::int64_t deviceId) { return "move:" + juce::String(deviceId); }

inline bool is(const juce::var& description, const char* kind)
{
    return description.toString().startsWith(juce::String(kind) + ":");
}
inline juce::String payload(const juce::var& description)
{
    return description.toString().fromFirstOccurrenceOf(":", false, false);
}
} // namespace drag

/** Audio file types the Sampler loads. */
inline bool isSampleFile(const juce::File& f)
{
    return f.hasFileExtension("wav;aif;aiff;flac;ogg;mp3");
}

using lattice::DeviceSpecs; // devices/framework/DeviceSpecs.h

/** One device parameter as an on-screen control: a knob for numbers, a menu for choices, a
    button for toggles. Edits go through the document (one undo step per drag); refresh() reads
    it back. Right-click offers what the owner adds (mapping to a rack macro). */
class ParameterControl final : public juce::Component
{
public:
    /** prototype: the parameter on a spec-only instance (DeviceSpecs), for its spec and range. */
    ParameterControl(SessionController& session, juce::ValueTree device, const DeviceParameter& prototype);

    void refresh();
    const ParameterSpec& spec() const noexcept { return parameterSpec; }
    /** The label above the control (a macro's own name); the parameter's name by default. */
    void setDisplayName(const juce::String& name)
    {
        displayName = name;
        repaint();
    }

    /** Adds the owner's items (with their actions) to the right-click menu: mapping to a rack's
        macros, for a device inside a rack. */
    std::function<void(juce::PopupMenu&)> extendMenu;

    static constexpr int width = 64;       // a knob or a toggle
    static constexpr int choiceWidth = 96; // a menu
    static constexpr int height = 72;
    int preferredWidth() const noexcept { return parameterSpec.kind == ParamKind::Choice ? choiceWidth : width; }

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;

private:
    void setValue(double plain);
    double documentValue() const;
    void showMenu();

    SessionController& session;
    juce::ValueTree device;
    const ParameterSpec parameterSpec;
    const juce::NormalisableRange<float> range;
    juce::String displayName;
    juce::Slider knob{juce::Slider::RotaryHorizontalVerticalDrag, juce::Slider::NoTextBox};
    juce::ComboBox choice;
    juce::TextButton toggle;
    juce::Label valueText;
    bool updating = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ParameterControl)
};

} // namespace lattice::debugui
