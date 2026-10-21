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
        if (stateNode == tree && (property == ids::sidechainTrack || property == ids::sidechainPreFx))
            markDirty(stateNode.getParent()); // a new route: the chain and the processing order change
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
        const auto state = node.getChildWithName(ids::deviceState);
        SidechainRoute route;
        route.trackId = static_cast<juce::int64>(state.getProperty(ids::sidechainTrack, static_cast<juce::int64>(0)));
        route.preFx = state.getProperty(ids::sidechainPreFx, false);
        out.sidechains.push_back(route);
        if (route.trackId != 0)
            build.sidechainSources.push_back(route);
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
    trackSidechains[LiveSet::idOf(owner)] = build.sidechainSources;

    // Nothing changed for the engine (e.g. only a name): keep the published chain, so nothing fades.
    // A new sidechain route with the same devices is a change (the track switches to it at once).
    const auto current = cache.processor->chain();
    const bool hasRacks = std::any_of(layout->racks.begin(), layout->racks.end(), [](const auto& r) { return r != nullptr; });
    if (current != nullptr && build.fresh.empty() && build.freshChains.empty() && !hasRacks &&
        current->devices == layout->devices && current->sidechains == layout->sidechains)
        return;

    engine.installChain(*cache.processor, build.fresh, build.freshChains, std::move(layout));
}

void SnapshotBuilder::setAutoArmTrack(std::int64_t trackId)
{
    if (trackId == autoArm)
        return;
    autoArm = trackId;
    if (!dirty)
    {
        dirty = true;
        triggerAsyncUpdate();
    }
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
        // A group track carries a stereo mix: its pan is a balance (unity at the centre), so
        // grouping tracks doesn't change their level. A MIDI track's pan is constant power.
        const auto [panL, panR] = LiveSet::isGroup(trackNode) ? balancePan(trackNode[ids::pan]) : constantPowerPan(trackNode[ids::pan]);
        ts.gainLeft = gain * panL;
        ts.gainRight = gain * panR;
        ts.mute = trackNode[ids::mute];
        ts.solo = trackNode[ids::solo];
        ts.arm = trackNode[ids::arm];
        ts.isGroup = LiveSet::isGroup(trackNode);
        const auto input = trackNode[ids::midiInput].toString();
        ts.inputSource = input.isEmpty() ? -1
                         : input == LiveSet::computerKeyboardInput ? static_cast<int>(computerKeyboardSource)
                                                                   : static_cast<int>(engine.midiSourceFor(input));
        ts.inputChannel = juce::jlimit(0, 16, static_cast<int>(trackNode[ids::midiChannel]));
        ts.monitoring = static_cast<Monitoring>(juce::jlimit(0, 2, static_cast<int>(trackNode[ids::monitoring])));
        ts.parent = set.parentGroupOf(t);
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

    // Auto-arm: the selected track counts as armed while nothing is armed by hand.
    if (autoArm != 0 && std::none_of(snapshot->tracks.begin(), snapshot->tracks.end(), [](const TrackSnapshot& ts) { return ts.arm; }))
        for (auto& ts : snapshot->tracks)
            if (ts.id == autoArm && !ts.isGroup)
                ts.arm = true;

    // Return tracks: a device chain each, fed by the sends.
    const int numReturns = std::min(set.numReturns(), TrackProcessor::maxReturns);
    std::vector<std::int64_t> returnIds;
    for (int r = 0; r < numReturns; ++r)
    {
        const auto node = set.returnTrack(r);
        ReturnSnapshot rs;
        rs.id = LiveSet::idOf(node);
        returnIds.push_back(rs.id);
        auto& cache = newTracks[rs.id];
        if (const auto it = tracks.find(rs.id); it != tracks.end())
            cache = std::move(it->second);
        bool chainDirty = allChainsDirty || dirtyChains.contains(rs.id);
        if (cache.processor == nullptr)
        {
            cache.processor = engine.createTrackProcessor(rs.id);
            chainDirty = true;
        }
        if (chainDirty)
            rebuildChain(node, cache);
        rs.processor = cache.processor;
        const float gain = gainFromDb(node[ids::volumeDb]);
        const auto [panL, panR] = constantPowerPan(node[ids::pan]);
        rs.gainLeft = gain * panL;
        rs.gainRight = gain * panR;
        rs.mute = node[ids::mute];
        rs.solo = node[ids::solo];
        rs.preFader = node[ids::preFader];
        snapshot->anyReturnSolo = snapshot->anyReturnSolo || rs.solo;
        snapshot->returns.push_back(std::move(rs));
    }

    // The master's chain (its cache key is 0: the master has no ID).
    {
        auto& cache = newTracks[0];
        if (const auto it = tracks.find(0); it != tracks.end())
            cache = std::move(it->second);
        bool chainDirty = allChainsDirty || dirtyChains.contains(0);
        if (cache.processor == nullptr)
        {
            cache.processor = engine.createTrackProcessor(0);
            chainDirty = true;
        }
        if (chainDirty)
            rebuildChain(set.master(), cache);
        snapshot->master = cache.processor;
    }

    // Sends: each track's level to each return.
    for (int t = 0; t < numTracks; ++t)
    {
        auto& ts = snapshot->tracks[static_cast<std::size_t>(t)];
        for (int r = 0; r < numReturns; ++r)
            ts.sendGains[static_cast<std::size_t>(r)] = gainFromDb(set.sendLevel(t, r));
    }

    // Delay compensation (docs/04-m3-devices.md, "Delay compensation"): tracks line up with the
    // slowest track, returns with the slowest return, and the tracks' direct path to the master
    // waits for the returns. With "reduced latency when monitoring", armed tracks skip their delay.
    snapshot->reducedLatencyWhenMonitoring = set.state().getProperty(ids::reducedLatencyWhenMonitoring, false);
    //
    // Group tracks (docs/05-m4-ui.md, 4b): a track's path to the master runs through its groups'
    // chains, so its path latency is its own plus theirs. Every path lines up with the slowest:
    // a track waits for the difference, and its group then adds the group's latency on top, so a
    // group itself never waits. A track's sends leave before its groups' chains, so they wait for
    // those chains' latency, and reach the returns in time with the direct path. Without groups
    // this is exactly the M3 rule.
    std::vector<int> groupsLatency(static_cast<std::size_t>(numTracks), 0); // the chains above each track
    for (int t = 0; t < numTracks; ++t)
    {
        const int p = snapshot->tracks[static_cast<std::size_t>(t)].parent;
        if (p >= 0 && p < t)
            groupsLatency[static_cast<std::size_t>(t)] =
                groupsLatency[static_cast<std::size_t>(p)] + snapshot->tracks[static_cast<std::size_t>(p)].processor->latencySamples();
    }
    int slowestTrack = 0;
    for (int t = 0; t < numTracks; ++t)
        slowestTrack = std::max(slowestTrack, snapshot->tracks[static_cast<std::size_t>(t)].processor->latencySamples() +
                                                  groupsLatency[static_cast<std::size_t>(t)]);
    for (int t = 0; t < numTracks; ++t)
    {
        auto& ts = snapshot->tracks[static_cast<std::size_t>(t)];
        const bool monitoring = snapshot->reducedLatencyWhenMonitoring && ts.arm;
        const int path = ts.processor->latencySamples() + groupsLatency[static_cast<std::size_t>(t)];
        ts.compensation = monitoring || ts.isGroup ? 0 : slowestTrack - path;
        ts.sendDelay = monitoring ? 0 : groupsLatency[static_cast<std::size_t>(t)];
    }

    // Mute and solo through groups: a muted group silences everything in it; a soloed track keeps
    // its groups audible, and a soloed group everything in it.
    {
        auto& tracksOut = snapshot->tracks;
        std::vector<bool> soloed(static_cast<std::size_t>(numTracks)), muted(static_cast<std::size_t>(numTracks));
        for (int t = 0; t < numTracks; ++t)
        {
            soloed[static_cast<std::size_t>(t)] = tracksOut[static_cast<std::size_t>(t)].solo;
            muted[static_cast<std::size_t>(t)] = tracksOut[static_cast<std::size_t>(t)].mute;
        }
        for (int t = 0; t < numTracks; ++t)
        {
            bool mute = muted[static_cast<std::size_t>(t)];
            bool solo = soloed[static_cast<std::size_t>(t)];
            for (int p = tracksOut[static_cast<std::size_t>(t)].parent; p >= 0; p = tracksOut[static_cast<std::size_t>(p)].parent)
            {
                mute = mute || muted[static_cast<std::size_t>(p)];
                solo = solo || soloed[static_cast<std::size_t>(p)];
            }
            if (soloed[static_cast<std::size_t>(t)])
                for (int p = tracksOut[static_cast<std::size_t>(t)].parent; p >= 0; p = tracksOut[static_cast<std::size_t>(p)].parent)
                    tracksOut[static_cast<std::size_t>(p)].solo = true; // its groups stay audible
            tracksOut[static_cast<std::size_t>(t)].mute = mute;
            if (solo)
                tracksOut[static_cast<std::size_t>(t)].solo = true;
        }
    }

    // The groups' levels, deepest first.
    {
        int deepestGroup = -1;
        std::vector<int> depth(static_cast<std::size_t>(numTracks), 0);
        for (int t = 0; t < numTracks; ++t)
        {
            const int p = snapshot->tracks[static_cast<std::size_t>(t)].parent;
            depth[static_cast<std::size_t>(t)] = p >= 0 && p < t ? depth[static_cast<std::size_t>(p)] + 1 : 0;
            if (snapshot->tracks[static_cast<std::size_t>(t)].isGroup)
                deepestGroup = std::max(deepestGroup, depth[static_cast<std::size_t>(t)]);
        }
        for (int d = deepestGroup; d >= 0; --d)
        {
            std::vector<int> level;
            for (int t = 0; t < numTracks; ++t)
                if (snapshot->tracks[static_cast<std::size_t>(t)].isGroup && depth[static_cast<std::size_t>(t)] == d)
                    level.push_back(t);
            if (!level.empty())
                snapshot->groupLevels.push_back(std::move(level));
        }
    }
    int slowestReturn = 0;
    for (const auto& rs : snapshot->returns)
        slowestReturn = std::max(slowestReturn, rs.processor->latencySamples());
    for (auto& rs : snapshot->returns)
        rs.compensation = slowestReturn - rs.processor->latencySamples();
    snapshot->masterInputDelay = slowestReturn;

    // Processing levels from the sidechains: a track comes after every track it listens to. Tracks
    // caught in a loop (the editor refuses those) go last, so nothing waits forever.
    {
        std::unordered_map<std::int64_t, int> indexOf;
        for (int t = 0; t < numTracks; ++t)
            if (!snapshot->tracks[static_cast<std::size_t>(t)].isGroup)
                indexOf[snapshot->tracks[static_cast<std::size_t>(t)].id] = t;
        std::vector<std::vector<int>> sources(static_cast<std::size_t>(numTracks));
        std::unordered_set<std::int64_t> tapped;
        for (int t = 0; t < numTracks; ++t)
            for (const auto& route : trackSidechains[snapshot->tracks[static_cast<std::size_t>(t)].id])
            {
                if (const auto it = indexOf.find(route.trackId); it != indexOf.end() && it->second != t)
                    sources[static_cast<std::size_t>(t)].push_back(it->second);
                if (route.preFx)
                    tapped.insert(route.trackId);
            }
        for (const auto& [id, cache] : newTracks)
            for (const auto& route : trackSidechains[id])
                if (route.preFx)
                    tapped.insert(route.trackId);
        std::vector<int> level(static_cast<std::size_t>(numTracks), -1);
        for (int pass = 0; pass <= numTracks; ++pass)
        {
            bool changed = false;
            for (int t = 0; t < numTracks; ++t)
            {
                if (level[static_cast<std::size_t>(t)] >= 0)
                    continue;
                int l = 0;
                bool ready = true;
                for (const int src : sources[static_cast<std::size_t>(t)])
                {
                    if (level[static_cast<std::size_t>(src)] < 0)
                    {
                        ready = false;
                        break;
                    }
                    l = std::max(l, level[static_cast<std::size_t>(src)] + 1);
                }
                if (ready)
                {
                    level[static_cast<std::size_t>(t)] = l;
                    changed = true;
                }
            }
            if (!changed)
                break;
        }
        int deepest = 0;
        for (const int l : level)
            deepest = std::max(deepest, l);
        for (auto& l : level)
            if (l < 0)
                l = deepest + 1; // in a loop
        for (int t = 0; t < numTracks; ++t)
        {
            if (snapshot->tracks[static_cast<std::size_t>(t)].isGroup)
                continue; // rendered after their members (groupLevels)
            const auto l = static_cast<std::size_t>(level[static_cast<std::size_t>(t)]);
            if (snapshot->levels.size() <= l)
                snapshot->levels.resize(l + 1);
            snapshot->levels[l].push_back(t);
        }
        for (const auto& [id, cache] : newTracks)
            cache.processor->setPreFxTapWanted(tapped.contains(id));
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
