#include "devices/racks/RackEditing.h"

#include "devices/instruments/Sampler.h"
#include "devices/racks/Rack.h"

namespace lattice::racks
{
namespace
{
juce::ValueTree stateOf(juce::ValueTree rack)
{
    auto state = rack.getChildWithName(ids::deviceState);
    if (!state.isValid())
    {
        state = juce::ValueTree(ids::deviceState);
        rack.appendChild(state, nullptr);
    }
    return state;
}

juce::ValueTree childOrNew(juce::ValueTree parent, const juce::Identifier& type, juce::UndoManager* um)
{
    auto child = parent.getChildWithName(type);
    if (!child.isValid())
    {
        child = juce::ValueTree(type);
        parent.appendChild(child, um);
    }
    return child;
}
} // namespace

juce::ValueTree insertRack(LiveSet& set, juce::ValueTree chain, int index, const juce::String& typeId)
{
    jassert(RackDevice::isRackType(typeId));
    auto rack = set.makeDevice(typeId);
    rack.appendChild(juce::ValueTree(ids::rackChains), nullptr);
    rack.appendChild(juce::ValueTree(ids::mappings), nullptr);
    rack.getChildWithName(ids::deviceState).setProperty("macroCount", RackDevice::defaultMacros, nullptr);
    return set.insertDeviceCopy(chain, index, rack);
}

juce::ValueTree addChain(LiveSet& set, juce::ValueTree rack, const juce::String& name)
{
    auto chains = childOrNew(rack, ids::rackChains, set.undoManager());
    juce::ValueTree chain(ids::rackChain);
    chain.setProperty(ids::id, static_cast<juce::int64>(set.allocateId()), nullptr);
    chain.setProperty(ids::name, name.isNotEmpty() ? name : "Chain " + juce::String(chains.getNumChildren() + 1), nullptr);
    chain.setProperty(ids::volumeDb, 0.0, nullptr);
    chain.setProperty(ids::pan, 0.0, nullptr);
    chain.setProperty(ids::mute, false, nullptr);
    chain.setProperty(ids::solo, false, nullptr);
    chain.appendChild(juce::ValueTree(ids::devices), nullptr);
    chains.appendChild(chain, set.undoManager());
    return chain;
}

void removeChain(LiveSet& set, const juce::ValueTree& chain)
{
    auto parent = chain.getParent();
    if (parent.isValid())
        parent.removeChild(chain, set.undoManager());
}

juce::ValueTree devicesOf(const juce::ValueTree& chain)
{
    return chain.getChildWithName(ids::devices);
}

void setChainMix(LiveSet& set, juce::ValueTree chain, double volumeDb, double pan, bool mute, bool solo)
{
    auto* um = set.undoManager();
    chain.setProperty(ids::volumeDb, volumeDb, um);
    chain.setProperty(ids::pan, pan, um);
    chain.setProperty(ids::mute, mute, um);
    chain.setProperty(ids::solo, solo, um);
}

void setKeyZone(LiveSet& set, juce::ValueTree chain, int low, int high, int fadeLow, int fadeHigh)
{
    auto* um = set.undoManager();
    chain.setProperty(ids::keyLow, low, um);
    chain.setProperty(ids::keyHigh, high, um);
    chain.setProperty(ids::keyFadeLow, fadeLow, um);
    chain.setProperty(ids::keyFadeHigh, fadeHigh, um);
}

void setVelocityZone(LiveSet& set, juce::ValueTree chain, int low, int high, int fadeLow, int fadeHigh)
{
    auto* um = set.undoManager();
    chain.setProperty(ids::velocityLow, low, um);
    chain.setProperty(ids::velocityHigh, high, um);
    chain.setProperty(ids::velocityFadeLow, fadeLow, um);
    chain.setProperty(ids::velocityFadeHigh, fadeHigh, um);
}

juce::ValueTree addDrumPad(LiveSet& set, juce::ValueTree drumRack, int note, const juce::String& samplePath)
{
    auto pad = padFor(drumRack, note);
    if (!pad.isValid())
    {
        pad = addChain(set, drumRack, juce::MidiMessage::getMidiNoteName(note, true, true, 4));
        pad.setProperty(ids::receiveNote, note, set.undoManager());
        pad.setProperty(ids::playNote, 60, set.undoManager());
        pad.setProperty(ids::chokeGroup, 0, set.undoManager());
    }
    auto devices = devicesOf(pad);
    juce::ValueTree sampler;
    for (const auto& d : devices)
        if (d[ids::deviceType].toString() == SamplerDevice::typeId)
            sampler = d;
    if (!sampler.isValid())
    {
        sampler = set.insertDevice(devices, 0, SamplerDevice::typeId);
        set.setDeviceParameter(sampler, "sample.mode", static_cast<double>(SamplerDevice::Mode::OneShot));
        set.setDeviceParameter(sampler, "sample.tuning", 1.0); // fixed: drums aren't retuned
    }
    if (samplePath.isNotEmpty())
        sampler.getChildWithName(ids::deviceState).setProperty("file", samplePath, set.undoManager());
    return pad;
}

juce::ValueTree padFor(const juce::ValueTree& drumRack, int note)
{
    for (const auto& chain : drumRack.getChildWithName(ids::rackChains))
        if (static_cast<int>(chain.getProperty(ids::receiveNote, -1)) == note)
            return chain;
    return {};
}

void setChokeGroup(LiveSet& set, juce::ValueTree pad, int group)
{
    pad.setProperty(ids::chokeGroup, juce::jlimit(0, 16, group), set.undoManager());
}

void setTunedPads(LiveSet& set, juce::ValueTree drumRack, bool tuned)
{
    stateOf(drumRack).setProperty("tunedPads", tuned, set.undoManager());
}

int macroCount(const juce::ValueTree& rack)
{
    return rack.getChildWithName(ids::deviceState).getProperty("macroCount", RackDevice::defaultMacros);
}

void setMacroCount(LiveSet& set, juce::ValueTree rack, int count)
{
    count = juce::jlimit(0, RackDevice::maxMacros, count);
    auto* um = set.undoManager();
    auto mappings = rack.getChildWithName(ids::mappings);
    for (int i = mappings.getNumChildren(); --i >= 0;)
        if (static_cast<int>(mappings.getChild(i)[ids::macro]) >= count)
            mappings.removeChild(i, um);
    stateOf(rack).setProperty("macroCount", count, um);
}

void setMacroName(LiveSet& set, juce::ValueTree rack, int macro, const juce::String& name)
{
    auto state = stateOf(rack);
    juce::ValueTree node;
    for (const auto& m : state)
        if (m.hasType("MACRO") && static_cast<int>(m["index"]) == macro)
            node = m;
    if (!node.isValid())
    {
        node = juce::ValueTree("MACRO");
        node.setProperty("index", macro, nullptr);
        state.appendChild(node, set.undoManager());
    }
    node.setProperty(ids::name, name, set.undoManager());
}

juce::ValueTree addMapping(LiveSet& set, juce::ValueTree rack, int macro, const juce::ValueTree& device,
                           const juce::String& parameterId, double minimum, double maximum, double curve)
{
    auto mappings = childOrNew(rack, ids::mappings, set.undoManager());
    juce::ValueTree m(ids::mapping);
    m.setProperty(ids::macro, macro, nullptr);
    m.setProperty(ids::targetDevice, static_cast<juce::int64>(LiveSet::idOf(device)), nullptr);
    m.setProperty(ids::parameter, parameterId, nullptr);
    m.setProperty(ids::minimum, minimum, nullptr);
    m.setProperty(ids::maximum, maximum, nullptr);
    m.setProperty(ids::curve, curve, nullptr);
    mappings.appendChild(m, set.undoManager());
    return m;
}

void removeMapping(LiveSet& set, const juce::ValueTree& mapping)
{
    auto parent = mapping.getParent();
    if (parent.isValid())
        parent.removeChild(mapping, set.undoManager());
}

int numMappings(const juce::ValueTree& rack)
{
    return rack.getChildWithName(ids::mappings).getNumChildren();
}

int storeVariation(LiveSet& set, juce::ValueTree rack, const juce::String& name)
{
    auto state = stateOf(rack);
    juce::StringArray values;
    for (int m = 0; m < RackDevice::maxMacros; ++m)
        values.add(juce::String(LiveSet::deviceParameter(rack, "macro" + juce::String(m + 1), 0.0), 17)); // exact
    juce::ValueTree v("VARIATION");
    v.setProperty(ids::name, name, nullptr);
    v.setProperty("values", values.joinIntoString(","), nullptr);
    state.appendChild(v, set.undoManager());
    return numVariations(rack) - 1;
}

void recallVariation(LiveSet& set, juce::ValueTree rack, int index)
{
    int n = 0;
    for (const auto& v : rack.getChildWithName(ids::deviceState))
    {
        if (!v.hasType("VARIATION"))
            continue;
        if (n++ != index)
            continue;
        const auto values = juce::StringArray::fromTokens(v["values"].toString(), ",", {});
        for (int m = 0; m < values.size() && m < RackDevice::maxMacros; ++m)
            set.setDeviceParameter(rack, "macro" + juce::String(m + 1), values[m].getDoubleValue());
        return;
    }
}

int numVariations(const juce::ValueTree& rack)
{
    int n = 0;
    for (const auto& v : rack.getChildWithName(ids::deviceState))
        if (v.hasType("VARIATION"))
            ++n;
    return n;
}

} // namespace lattice::racks
