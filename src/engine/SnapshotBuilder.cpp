#include "engine/SnapshotBuilder.h"

#include "devices/racks/Rack.h"
#include "engine/AudioEngine.h"

#include <algorithm>
#include <functional>

namespace lattice
{
namespace
{
float gainFromDb(double db)
{
    return db <= LiveSet::minVolumeDb ? 0.0f
                                      : juce::Decibels::decibelsToGain(static_cast<float>(db), -1000.0f);
}
} // namespace

SnapshotBuilder::SnapshotBuilder(LiveSet& s, AudioEngine& e, TuningResolver resolver, const DeviceRegistry& r)
    : set(s), engine(e), resolveTuning(std::move(resolver)), registry(r)
{
    set.state().addListener(this);
}

SnapshotBuilder::~SnapshotBuilder()
{
    cancelPendingUpdate();
    set.state().removeListener(this);
}

void SnapshotBuilder::markDirty(const juce::ValueTree& changed)
{
    for (auto node = changed; node.isValid(); node = node.getParent())
    {
        if (node.hasType(ids::clip))
        {
            dirtyClips.insert(LiveSet::idOf(node));
            break;
        }
        if (node.hasType(ids::devices))
        {
            if (const auto owner = LiveSet::chainOwnerOf(node); owner.isValid())
                dirtyChains.insert(LiveSet::idOf(owner));
            break;
        }
    }
    if (!dirty)
    {
        dirty = true;
        triggerAsyncUpdate();
    }
}

void SnapshotBuilder::markClipsIn(const juce::ValueTree& tree)
{
    if (tree.hasType(ids::clip))
    {
        dirtyClips.insert(LiveSet::idOf(tree));
        return;
    }
    for (const auto& child : tree)
        markClipsIn(child);
}

juce::ValueTree SnapshotBuilder::enclosingDeviceState(const juce::ValueTree& node)
{
    for (auto n = node; n.isValid(); n = n.getParent())
    {
        if (n.hasType(ids::deviceState) && n.getParent().hasType(ids::device))
            return n;
        if (n.hasType(ids::device) || n.hasType(ids::devices) || n.hasType(ids::track))
            break;
    }
    return {};
}

void SnapshotBuilder::applyChainMix(const juce::ValueTree& chainNode, RackChainState& state)
{
    state.setMix(chainNode.getProperty(ids::volumeDb, 0.0), chainNode.getProperty(ids::pan, 0.0),
                 chainNode.getProperty(ids::mute, false), chainNode.getProperty(ids::solo, false));
}

void SnapshotBuilder::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    // A rack chain's mix is live, like a parameter: no rebuild.
    if (tree.hasType(ids::rackChain) &&
        (property == ids::volumeDb || property == ids::pan || property == ids::mute || property == ids::solo))
    {
        const auto chainId = LiveSet::idOf(tree);
        for (auto& [trackId, cache] : tracks)
            if (const auto it = cache.rackChains.find(chainId); it != cache.rackChains.end())
                applyChainMix(tree, *it->second);
        return;
    }
    // Device values go straight to the instance: nothing is rebuilt.
    if (tree.hasType(ids::params) && tree.getParent().hasType(ids::device))
    {
        forwardParameter(tree, property);
        return;
    }
    if (tree.hasType(ids::device) && property == ids::enabled)
    {
        if (auto* device = deviceFor(LiveSet::idOf(tree)))
            device->setEnabled(tree.getProperty(ids::enabled, true));
        return;
    }
    if (const auto stateNode = enclosingDeviceState(tree); stateNode.isValid())
    {
        forwardState(stateNode);
        // A rack's macro count and tuned pads change its structure: rebuild.
        if (RackDevice::isRackType(stateNode.getParent()[ids::deviceType].toString()) && stateNode == tree &&
            (property == juce::Identifier("macroCount") || property == juce::Identifier("tunedPads")))
            markDirty(stateNode.getParent());
        return;
    }
    markDirty(tree);
}

void SnapshotBuilder::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child)
{
    if (const auto stateNode = enclosingDeviceState(parent); stateNode.isValid())
    {
        forwardState(stateNode);
        return;
    }
    // A subtree that arrives whole (undo, paste, a loaded set) may reuse IDs of cached clips.
    markClipsIn(child);
    markDirty(parent);
}

void SnapshotBuilder::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int)
{
    if (const auto stateNode = enclosingDeviceState(parent); stateNode.isValid())
    {
        forwardState(stateNode);
        return;
    }
    markDirty(parent);
}

void SnapshotBuilder::valueTreeRedirected(juce::ValueTree&)
{
    clipCache.clear();
    allChainsDirty = true;
    markDirty(set.state());
}

void SnapshotBuilder::forgetAll()
{
    tracks.clear();
    clipCache.clear();
    dirtyClips.clear();
    deviceIndex.clear();
    allChainsDirty = true;
    markDirty(set.state());
}

void SnapshotBuilder::forwardParameter(const juce::ValueTree& values, const juce::Identifier& parameterId)
{
    auto* device = deviceFor(LiveSet::idOf(values.getParent()));
    if (device == nullptr)
        return; // not built yet: it reads the document when it is
    if (auto* p = device->findParameter(parameterId.toString()))
    {
        const auto* v = values.getPropertyPointer(parameterId);
        p->setBase(v != nullptr ? static_cast<float>(static_cast<double>(*v)) : p->spec().defaultValue);
    }
}

void SnapshotBuilder::forwardState(const juce::ValueTree& stateNode)
{
    if (auto* device = deviceFor(LiveSet::idOf(stateNode.getParent())))
        device->setState(stateNode);
}

void SnapshotBuilder::applyDocument(const juce::ValueTree& deviceNode, StudioDevice& device)
{
    for (int i = 0; i < device.numParameters(); ++i)
    {
        auto& p = device.parameter(i);
        p.setBase(static_cast<float>(LiveSet::deviceParameter(deviceNode, p.id(), p.spec().defaultValue)));
    }
    device.setEnabled(deviceNode.getProperty(ids::enabled, true));
    device.setState(deviceNode.getChildWithName(ids::deviceState));
}

StudioDevice* SnapshotBuilder::deviceFor(std::int64_t deviceId) const
{
    const auto it = deviceIndex.find(deviceId);
    return it != deviceIndex.end() ? it->second : nullptr;
}

void SnapshotBuilder::buildChain(const juce::ValueTree& devicesNode, DeviceChainLayout& out, ChainBuild& build,
                                 int depth)
{
    for (const auto& node : devicesNode)
    {
        if (!node.hasType(ids::device))
            continue;
        const auto deviceId = LiveSet::idOf(node);
        const auto type = node[ids::deviceType].toString();
        const bool isRack = RackDevice::isRackType(type);
        if (isRack && depth >= RackDevice::maxDepth)
            continue; // racks nest at most maxDepth deep (the editor refuses deeper ones)

        DevicePtr device;
        if (const auto it = build.cache->devices.find(deviceId); it != build.cache->devices.end() && it->second->typeId() == type)
            device = it->second;
        if (device == nullptr || build.kept.contains(deviceId)) // a duplicated ID gets its own instance
        {
            device = DevicePtr(registry.create(type, build.context));
            applyDocument(node, *device);
            build.fresh.push_back(device);
        }
        build.kept[deviceId] = device;
        out.devices.push_back(device);
        out.racks.push_back(isRack ? buildRack(node, build, depth + 1) : nullptr);
    }
}

std::shared_ptr<RackLayout> SnapshotBuilder::buildRack(const juce::ValueTree& rackNode, ChainBuild& build, int depth)
{
    auto rack = std::make_shared<RackLayout>();
    rack->kind = RackDevice::kindForType(rackNode[ids::deviceType].toString());
    rack->tunedPads = rackNode.getChildWithName(ids::deviceState).getProperty("tunedPads", false);

    for (const auto& chainNode : rackNode.getChildWithName(ids::rackChains))
    {
        if (!chainNode.hasType(ids::rackChain))
            continue;
        RackChainLayout c;
        c.id = LiveSet::idOf(chainNode);
        c.keyLow = chainNode.getProperty(ids::keyLow, 0);
        c.keyHigh = chainNode.getProperty(ids::keyHigh, 127);
        c.keyFadeLow = chainNode.getProperty(ids::keyFadeLow, 0);
        c.keyFadeHigh = chainNode.getProperty(ids::keyFadeHigh, 0);
        c.velocityLow = chainNode.getProperty(ids::velocityLow, 1);
        c.velocityHigh = chainNode.getProperty(ids::velocityHigh, 127);
        c.velocityFadeLow = chainNode.getProperty(ids::velocityFadeLow, 0);
        c.velocityFadeHigh = chainNode.getProperty(ids::velocityFadeHigh, 0);
        c.receiveNote = chainNode.getProperty(ids::receiveNote, -1);
        c.playNote = chainNode.getProperty(ids::playNote, 60);
        c.chokeGroup = chainNode.getProperty(ids::chokeGroup, 0);

        // The chain's working state carries over by chain ID.
        if (const auto it = build.cache->rackChains.find(c.id); it != build.cache->rackChains.end() &&
                                                               !build.keptChains.contains(c.id))
            c.state = it->second;
        if (c.state == nullptr)
        {
            c.state = std::make_shared<RackChainState>();
            build.freshChains.push_back(c.state);
        }
        build.keptChains[c.id] = c.state;
        applyChainMix(chainNode, *c.state);
        buildChain(chainNode.getChildWithName(ids::devices), c.chain, build, depth);
        rack->chains.push_back(std::move(c));
    }

    // Macro mappings, to parameters of devices inside this rack.
    std::unordered_set<std::int64_t> inside;
    std::function<void(const juce::ValueTree&)> collect = [&](const juce::ValueTree& node) {
        if (node.hasType(ids::device))
            inside.insert(LiveSet::idOf(node));
        for (const auto& child : node)
            if (!child.hasType(ids::params) && !child.hasType(ids::deviceState))
                collect(child);
    };
    collect(rackNode.getChildWithName(ids::rackChains));
    const int macroCount = rackNode.getChildWithName(ids::deviceState).getProperty("macroCount", RackDevice::defaultMacros);
    for (const auto& m : rackNode.getChildWithName(ids::mappings))
    {
        const std::int64_t target = static_cast<juce::int64>(m[ids::targetDevice]);
        const int macro = m[ids::macro];
        if (!inside.contains(target) || macro < 0 || macro >= macroCount)
            continue;
        const auto it = build.kept.find(target);
        if (it == build.kept.end())
            continue;
        if (auto* p = it->second->findParameter(m[ids::parameter].toString()); p != nullptr && p->spec().modulatable)
        {
            MacroMapping mapping;
            mapping.macro = macro;
            mapping.target = p;
            mapping.minimum = static_cast<float>(static_cast<double>(m.getProperty(ids::minimum, 0.0)));
            mapping.maximum = static_cast<float>(static_cast<double>(m.getProperty(ids::maximum, 1.0)));
            mapping.curve = static_cast<float>(static_cast<double>(m.getProperty(ids::curve, 0.0)));
            rack->mappings.push_back(mapping);
        }
    }
    return rack;
}

void SnapshotBuilder::rebuildChain(const juce::ValueTree& owner, TrackCache& cache)
{
    auto layout = std::make_shared<DeviceChainLayout>();
    ChainBuild build;
    build.cache = &cache;
    build.context.tuning = &cache.processor->tuning();
    buildChain(LiveSet::chainOf(owner), *layout, build, 0);
    cache.devices = std::move(build.kept);
    cache.rackChains = std::move(build.keptChains);

    // Nothing changed for the engine (e.g. only a name): keep the published chain, so nothing fades.
    const auto current = cache.processor->chain();
    const bool hasRacks = std::any_of(layout->racks.begin(), layout->racks.end(), [](const auto& r) { return r != nullptr; });
    if (current != nullptr && build.fresh.empty() && build.freshChains.empty() && !hasRacks &&
        current->devices == layout->devices)
        return;

    engine.installChain(*cache.processor, build.fresh, build.freshChains, std::move(layout));
}

void SnapshotBuilder::handleAsyncUpdate()
{
    rebuildIfNeeded();
}

void SnapshotBuilder::rebuildIfNeeded()
{
    if (dirty)
        rebuildNow();
}

std::shared_ptr<TrackProcessor> SnapshotBuilder::processorFor(std::int64_t trackId) const
{
    const auto it = tracks.find(trackId);
    return it != tracks.end() ? it->second.processor : nullptr;
}

void SnapshotBuilder::rebuildNow()
{
    cancelPendingUpdate();
    dirty = false;
    lastClipsBuilt = 0;

    auto snapshot = std::make_shared<EngineSnapshot>();
    snapshot->generation = ++lastGeneration;
    snapshot->tempo = set.tempo();
    snapshot->timeSigNum = set.timeSigNumerator();
    snapshot->timeSigDen = set.timeSigDenominator();
    snapshot->globalQuantization = set.globalQuantization();
    snapshot->metronomeOn = set.metronomeOn();
    snapshot->metronomeGain = gainFromDb(set.metronomeVolumeDb());

    const auto masterNode = set.master();
    const float masterGain = gainFromDb(masterNode[ids::volumeDb]);
    const auto [masterPanL, masterPanR] = constantPowerPan(masterNode[ids::pan]);
    snapshot->masterGainLeft = masterGain * masterPanL;
    snapshot->masterGainRight = masterGain * masterPanR;

    const int numScenes = set.numScenes();
    snapshot->scenes.reserve(static_cast<std::size_t>(numScenes));
    for (int s = 0; s < numScenes; ++s)
    {
        const auto scene = set.scene(s);
        SceneSnapshot sc;
        sc.tempo = scene[ids::sceneTempo];
        sc.timeSigNum = scene[ids::sceneTimeSigNum];
        sc.timeSigDen = scene[ids::sceneTimeSigDen];
        snapshot->scenes.push_back(sc);
    }

    std::unordered_map<std::int64_t, ClipDataPtr> newClipCache;
    std::unordered_map<std::int64_t, TrackCache> newTracks;
    const int numTracks = set.numTracks();
    snapshot->tracks.reserve(static_cast<std::size_t>(numTracks));

    for (int t = 0; t < numTracks; ++t)
    {
        const auto trackNode = set.track(t);
        TrackSnapshot ts;
        ts.id = LiveSet::idOf(trackNode);

        TrackCache cache;
        if (const auto it = tracks.find(ts.id); it != tracks.end())
            cache = std::move(it->second);
        bool chainDirty = allChainsDirty || dirtyChains.contains(ts.id);
        if (cache.processor == nullptr)
        {
            cache.processor = engine.createTrackProcessor(ts.id);
            chainDirty = true;
        }

        // Tuning: re-resolve every time (a tuning file may have been loaded since), publish on change.
        const juce::String tuningId = trackNode[ids::tuningId].toString();
        auto tuning = resolveTuning(tuningId);
        if (tuning != cache.tuning || tuningId != cache.tuningId)
        {
            cache.processor->tuning().setTuning(tuning);
            cache.tuning = tuning;
            cache.tuningId = tuningId;
        }
        cache.processor->tuning().setNoteOffset(trackNode[ids::noteOffset]);
        cache.processor->tuning().setHighNotePolicy(static_cast<int>(trackNode[ids::highNotePolicy]) == 1
                                                        ? HighNotePolicy::FoldDown
                                                        : HighNotePolicy::Mute);
        if (chainDirty)
            rebuildChain(trackNode, cache);

        ts.processor = cache.processor;
        const float gain = gainFromDb(trackNode[ids::volumeDb]);
        const auto [panL, panR] = constantPowerPan(trackNode[ids::pan]);
        ts.gainLeft = gain * panL;
        ts.gainRight = gain * panR;
        ts.mute = trackNode[ids::mute];
        ts.solo = trackNode[ids::solo];
        ts.arm = trackNode[ids::arm];
        snapshot->anySolo = snapshot->anySolo || ts.solo;

        ts.slots.resize(static_cast<std::size_t>(numScenes));
        for (int s = 0; s < numScenes; ++s)
        {
            const auto slotNode = set.slot(t, s);
            auto& slot = ts.slots[static_cast<std::size_t>(s)];
            slot.hasStopButton = slotNode[ids::hasStopButton];
            const auto clipNode = slotNode.getChildWithName(ids::clip);
            if (!clipNode.isValid())
                continue;

            const auto clipId = LiveSet::idOf(clipNode);
            ClipDataPtr data;
            if (const auto it = clipCache.find(clipId); it != clipCache.end() && !dirtyClips.contains(clipId))
                data = it->second;
            if (data == nullptr)
            {
                data = ClipData::fromTree(clipNode);
                ++lastClipsBuilt;
            }
            newClipCache[clipId] = data;
            slot.clip = std::move(data);
        }

        newTracks[ts.id] = std::move(cache);
        snapshot->tracks.push_back(std::move(ts));
    }

    clipCache = std::move(newClipCache);
    dirtyClips.clear();
    dirtyChains.clear();
    allChainsDirty = false;
    tracks = std::move(newTracks);
    deviceIndex.clear();
    for (const auto& [trackId, cache] : tracks)
        for (const auto& [deviceId, device] : cache.devices)
            deviceIndex[deviceId] = device.get();

    engine.publishSnapshot(std::move(snapshot));
}

void SnapshotBuilder::collectGarbage()
{
    engine.collectGarbage();
    for (auto& [id, cache] : tracks)
        cache.processor->collectGarbage();
}

} // namespace lattice
