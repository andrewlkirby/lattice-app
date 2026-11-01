#pragma once

#include "core/model/LiveSet.h"

#include <juce_data_structures/juce_data_structures.h>

namespace lattice
{
class DeviceRegistry;

/** The names of undo steps (docs/05-m4-ui.md, 4h): what was done, then where, in Find's location
    format, so two knobs of the same name on different tracks or devices read differently:

        "Cutoff → 1.20 kHz · Synth › Filter · 2 MIDI"
        "Rename chain · Chain 2 · Instrument Rack · 1 MIDI"
        "Tempo → 128.00"                                   (the whole set: no location)

    The location is made from the edited node's place in the set when the edit is made, and never
    changes after (renaming the track later leaves the step as it was). */
namespace undoNames
{
/** " › ": a device and its parameter group ("Synth › Filter"). */
juce::String arrow();
/** " · ": between the action and each level of the location, the innermost first. */
juce::String dot();

/** Where a node of the set is: a device, a rack chain or pad, a clip, a slot, a scene, a track, a
    return, the master; or anything inside one of those (a device's parameters or state, a clip's
    notes, a rack's mappings). `group`, if any, follows the innermost device or clip after the
    arrow. Empty for the set itself and for nodes outside the set. */
juce::String locationOf(const juce::ValueTree& node, const DeviceRegistry& registry, const juce::String& group = {});

/** A device's name: the user's, or its type's ("Synth"). */
juce::String deviceName(const juce::ValueTree& device, const DeviceRegistry& registry);

/** "action · location", or the action alone when there's no location. */
juce::String make(const juce::String& action, const juce::String& location);
/** The action with where `node` is. */
juce::String make(const juce::String& action, const juce::ValueTree& node, const DeviceRegistry& registry);
/** One control set to a value: "Cutoff → 1.20 kHz". */
juce::String withValue(const juce::String& control, const juce::String& valueText);

/** A step's name split into what was done and where (empty if it has no location). */
struct Parts
{
    juce::String action, location;
};
Parts split(const juce::String& name);
} // namespace undoNames

} // namespace lattice
