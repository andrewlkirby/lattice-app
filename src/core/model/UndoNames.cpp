#include "core/model/UndoNames.h"

#include "devices/framework/DeviceRegistry.h"
#include "devices/racks/Rack.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace lattice::undoNames
{

juce::String arrow()
{
    static const auto s = juce::String::fromUTF8(" \xe2\x80\xba ");
    return s;
}

juce::String dot()
{
    static const auto s = juce::String::fromUTF8(" \xc2\xb7 ");
    return s;
}

juce::String deviceName(const juce::ValueTree& device, const DeviceRegistry& registry)
{
    if (const auto own = device[ids::name].toString(); own.isNotEmpty())
        return own;
    const auto type = device[ids::deviceType].toString();
    const auto* info = registry.find(type);
    return info != nullptr ? info->name : type + " (missing)";
}

namespace
{
juce::String sceneName(const juce::ValueTree& scene, int index)
{
    const auto own = scene[ids::name].toString();
    return own.isNotEmpty() ? own : "Scene " + juce::String(index + 1);
}

/** A rack chain's name; a Drum Rack's pads by their note ("Pad C1"). */
juce::String chainName(const juce::ValueTree& chain)
{
    const auto rack = chain.getParent().getParent();
    if (rack[ids::deviceType].toString() == RackDevice::drumRackId && chain.hasProperty(ids::receiveNote))
        return "Pad " + juce::MidiMessage::getMidiNoteName(static_cast<int>(chain[ids::receiveNote]), true, true, 4);
    return chain[ids::name].toString();
}
} // namespace

juce::String locationOf(const juce::ValueTree& node, const DeviceRegistry& registry, const juce::String& group)
{
    juce::StringArray levels; // the innermost first
    // The group belongs to the edited device or clip itself, not to one it's inside.
    const auto withGroup = [&](juce::String level) {
        if (levels.isEmpty() && group.isNotEmpty())
            level << arrow() << group;
        return level;
    };
    for (auto n = node; n.isValid(); n = n.getParent())
    {
        if (n.hasType(ids::device))
            levels.add(withGroup(deviceName(n, registry)));
        else if (n.hasType(ids::rackChain))
            levels.add(chainName(n));
        else if (n.hasType(ids::clip))
        {
            const auto own = n[ids::name].toString();
            levels.add(withGroup(own.isNotEmpty() ? own : juce::String("Clip")));
        }
        else if (n.hasType(ids::slot))
        {
            const int index = n.getParent().indexOf(n);
            levels.add(sceneName(n.getRoot().getChildWithName(ids::scenes).getChild(index), index));
        }
        else if (n.hasType(ids::scene))
        {
            levels.add(sceneName(n, n.getParent().indexOf(n)));
            break;
        }
        else if (n.hasType(ids::track) || n.hasType(ids::returnTrack))
        {
            levels.add(n[ids::name].toString());
            break;
        }
        else if (n.hasType(ids::master))
        {
            levels.add("Master");
            break;
        }
    }
    levels.removeEmptyStrings();
    return levels.joinIntoString(dot());
}

juce::String make(const juce::String& action, const juce::String& location)
{
    return location.isNotEmpty() ? action + dot() + location : action;
}

juce::String make(const juce::String& action, const juce::ValueTree& node, const DeviceRegistry& registry)
{
    return make(action, locationOf(node, registry));
}

juce::String withValue(const juce::String& control, const juce::String& valueText)
{
    return valueText.isNotEmpty() ? control + juce::String::fromUTF8(" \xe2\x86\x92 ") + valueText : control;
}

Parts split(const juce::String& name)
{
    const int at = name.indexOf(dot());
    if (at < 0)
        return {name, {}};
    return {name.substring(0, at), name.substring(at + dot().length())};
}

} // namespace lattice::undoNames
