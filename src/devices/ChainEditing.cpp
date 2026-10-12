#include "devices/ChainEditing.h"

#include "devices/racks/Rack.h"

#include <algorithm>

namespace lattice::chains
{

namespace
{
/** The rack a chain belongs to (DEVICE / CHAINS / CHAIN / DEVICES), or an invalid tree. */
juce::ValueTree rackOf(const juce::ValueTree& chain)
{
    const auto owner = chain.getParent();
    if (!owner.hasType(ids::rackChain))
        return {};
    return owner.getParent().getParent();
}

/** How many racks deep the deepest rack inside a device (itself included) goes: 0 for a plain
    device, 1 for a rack with no racks inside. */
int rackHeight(const juce::ValueTree& device)
{
    if (!RackDevice::isRackType(device[ids::deviceType].toString()))
        return 0;
    int inner = 0;
    for (const auto& chain : device.getChildWithName(ids::rackChains))
        for (const auto& child : chain.getChildWithName(ids::devices))
            inner = std::max(inner, rackHeight(child));
    return 1 + inner;
}
} // namespace

Role roleOf(const juce::String& typeId, const DeviceRegistry& registry)
{
    if (RackDevice::isRackType(typeId))
    {
        switch (RackDevice::kindForType(typeId))
        {
        case RackKind::Instrument:
        case RackKind::Drum:
            return Role::Instrument;
        case RackKind::MidiEffect:
            return Role::MidiEffect;
        case RackKind::AudioEffect:
            return Role::AudioEffect;
        }
    }
    switch (registry.kindOf(typeId))
    {
    case DeviceKind::MidiEffect:
        return Role::MidiEffect;
    case DeviceKind::Instrument:
        return Role::Instrument;
    case DeviceKind::AudioEffect:
    case DeviceKind::Rack:
        break;
    }
    return Role::AudioEffect;
}

bool carriesMidi(const juce::ValueTree& chain)
{
    const auto owner = chain.getParent();
    if (owner.hasType(ids::track))
        return true;
    if (owner.hasType(ids::rackChain))
    {
        const auto kind = RackDevice::kindForType(rackOf(chain)[ids::deviceType].toString());
        return kind != RackKind::AudioEffect;
    }
    return false; // a return's or the master's chain
}

int depthOf(const juce::ValueTree& chain)
{
    int depth = 0;
    for (auto rack = rackOf(chain); rack.isValid(); rack = rackOf(rack.getParent()))
        ++depth;
    return depth;
}

Placement place(const juce::ValueTree& chain, const juce::String& typeId, int desiredIndex,
                const DeviceRegistry& registry, const juce::ValueTree& device)
{
    Placement p;
    if (!chain.hasType(ids::devices))
    {
        p.reason = "Not a device chain";
        return p;
    }
    const auto role = roleOf(typeId, registry);
    if (!carriesMidi(chain) && role != Role::AudioEffect)
    {
        p.reason = "Only audio effects can go here";
        return p;
    }
    if (device.isValid())
    {
        // Not into itself, and not deeper than racks may nest.
        for (auto n = chain; n.isValid(); n = n.getParent())
            if (n == device)
            {
                p.reason = "A rack can't go inside itself";
                return p;
            }
    }
    const int height = device.isValid() ? rackHeight(device) : (RackDevice::isRackType(typeId) ? 1 : 0);
    if (height > 0 && depthOf(chain) + height > RackDevice::maxDepth)
    {
        p.reason = "Racks can only be nested " + juce::String(RackDevice::maxDepth) + " deep";
        return p;
    }

    // The chain as it would be without the device being moved: its zones.
    std::vector<std::pair<Role, juce::ValueTree>> others;
    int ownIndex = -1;
    for (int i = 0; i < chain.getNumChildren(); ++i)
    {
        const auto child = chain.getChild(i);
        if (child == device)
        {
            ownIndex = i;
            continue;
        }
        others.emplace_back(roleOf(child[ids::deviceType].toString(), registry), child);
    }
    const int count = static_cast<int>(others.size());
    int midiEnd = 0; // MIDI effects are [0, midiEnd)
    while (midiEnd < count && others[static_cast<std::size_t>(midiEnd)].first == Role::MidiEffect)
        ++midiEnd;
    int audioStart = midiEnd; // audio effects are [audioStart, count)
    juce::ValueTree instrument;
    if (audioStart < count && others[static_cast<std::size_t>(audioStart)].first == Role::Instrument)
        instrument = others[static_cast<std::size_t>(audioStart++)].second;

    // desiredIndex counts the chain as it is (with the moved device); make it count `others`.
    int want = desiredIndex < 0 || desiredIndex > chain.getNumChildren() ? count : desiredIndex;
    if (ownIndex >= 0 && want > ownIndex)
        --want;

    int index = 0;
    switch (role)
    {
    case Role::MidiEffect:
        index = std::clamp(want, 0, midiEnd);
        break;
    case Role::Instrument:
        index = midiEnd;
        if (instrument.isValid())
            p.replaces = instrument;
        break;
    case Role::AudioEffect:
        index = std::clamp(want, audioStart, count);
        break;
    }
    p.allowed = true;
    p.index = index;
    return p;
}

namespace
{
/** Removes the instrument a new one replaces. The new one takes its place, so the index holds. */
int makeRoom(LiveSet& set, const Placement& p)
{
    if (p.replaces.isValid())
        set.removeDevice(p.replaces);
    return p.index;
}
} // namespace

juce::ValueTree insert(LiveSet& set, juce::ValueTree chain, const juce::String& typeId, int desiredIndex,
                       const DeviceRegistry& registry)
{
    const auto p = place(chain, typeId, desiredIndex, registry);
    if (!p.allowed)
        return {};
    return set.insertDevice(chain, makeRoom(set, p), typeId);
}

juce::ValueTree insertCopy(LiveSet& set, juce::ValueTree chain, const juce::ValueTree& device, int desiredIndex,
                           const DeviceRegistry& registry)
{
    const auto type = device[ids::deviceType].toString();
    const auto p = place(chain, type, desiredIndex, registry);
    if (!p.allowed)
        return {};
    // A preset of a rack nests as deep as it is.
    if (const int height = rackHeight(device); height > 0 && depthOf(chain) + height > RackDevice::maxDepth)
        return {};
    return set.insertDeviceCopy(chain, makeRoom(set, p), device);
}

bool move(LiveSet& set, const juce::ValueTree& device, juce::ValueTree toChain, int desiredIndex,
          const DeviceRegistry& registry)
{
    const auto p = place(toChain, device[ids::deviceType].toString(), desiredIndex, registry, device);
    if (!p.allowed)
        return false;
    makeRoom(set, p);
    if (device.getParent() == toChain && toChain.indexOf(device) == p.index)
        return true; // already there
    set.moveDevice(device, toChain, p.index); // within a chain: the final index; else where it goes in
    return true;
}

namespace
{
/** Every track ID the devices under `node` listen to. */
void listenedTo(const juce::ValueTree& node, std::vector<std::int64_t>& out)
{
    if (node.hasType(ids::deviceState))
        if (const auto id = static_cast<std::int64_t>(static_cast<juce::int64>(node.getProperty(ids::sidechainTrack, 0)));
            id != 0)
            out.push_back(id);
    if (node.hasType(ids::slots) || node.hasType(ids::params))
        return;
    for (const auto& child : node)
        listenedTo(child, out);
}

/** True if track `from` reaches track `to` by following sidechain sources. */
bool reaches(const LiveSet& set, std::int64_t from, std::int64_t to)
{
    std::vector<std::int64_t> pending{from}, seen;
    while (!pending.empty())
    {
        const auto t = pending.back();
        pending.pop_back();
        if (t == to)
            return true;
        if (std::find(seen.begin(), seen.end(), t) != seen.end())
            continue;
        seen.push_back(t);
        const int index = set.trackIndexForId(t);
        if (index >= 0)
            listenedTo(LiveSet::chainOf(set.track(index)), pending);
    }
    return false;
}
} // namespace

std::vector<std::int64_t> sidechainSources(const LiveSet& set, const juce::ValueTree& device)
{
    const auto owner = LiveSet::chainOwnerOf(device);
    const bool onTrack = owner.hasType(ids::track);
    const auto own = onTrack ? LiveSet::idOf(owner) : std::int64_t{0};
    std::vector<std::int64_t> out;
    for (int t = 0; t < set.numTracks(); ++t)
    {
        const auto id = set.trackId(t);
        if (onTrack && (id == own || reaches(set, id, own)))
            continue;
        out.push_back(id);
    }
    return out;
}

bool setSidechainSource(LiveSet& set, juce::ValueTree device, std::int64_t trackId, bool preFx)
{
    auto state = device.getChildWithName(ids::deviceState);
    if (!state.isValid())
    {
        state = juce::ValueTree(ids::deviceState);
        device.appendChild(state, set.undoManager());
    }
    if (trackId != 0)
    {
        const auto allowed = sidechainSources(set, device);
        if (std::find(allowed.begin(), allowed.end(), trackId) == allowed.end())
            return false;
    }
    state.setProperty(ids::sidechainTrack, static_cast<juce::int64>(trackId), set.undoManager());
    state.setProperty(ids::sidechainPreFx, preFx, set.undoManager());
    return true;
}

} // namespace lattice::chains
