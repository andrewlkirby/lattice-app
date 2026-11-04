#pragma once

#include "devices/framework/DeviceSpecs.h"
#include "ui/widgets/ValueBinding.h"

#include <memory>

namespace lattice::ui
{

/** A binding for a value named by its address (ValueBinding::target): "device:<id>/<parameter>",
    "node:<id>/<property>" (a track's or return's volume, pan, mute, solo, arm, pre-fader),
    "master/<property>", "send:<track id>/<return id>", "clip:<id>/<value>" (clipValues),
    "notes:<clip id>/..." (the notes on held steps: steps::target). MIDI Map and the controllers' encoders
    (docs/06-m5-controllers.md, 5c) both edit through these, so a value moved from either is the
    same undo step a mouse drag makes. Null if the address names nothing in the set. */
std::unique_ptr<ValueBinding> bindingForTarget(LiveSet& set, DeviceSpecs& specs, juce::UndoManager* undo,
                                               const juce::String& target);

/** The device node with a document ID anywhere under node (racks' chains too), or an invalid tree. */
juce::ValueTree findDeviceById(const juce::ValueTree& node, std::int64_t id);

/** "device:<id>/<parameter>" */
inline juce::String deviceTarget(const juce::ValueTree& device, const juce::String& parameterId)
{
    return "device:" + juce::String(LiveSet::idOf(device)) + "/" + parameterId;
}
/** "send:<track id>/<return id>" */
inline juce::String sendTarget(std::int64_t trackId, std::int64_t returnId)
{
    return "send:" + juce::String(trackId) + "/" + juce::String(returnId);
}

} // namespace lattice::ui
