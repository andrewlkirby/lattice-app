#include "engine/SnapshotBuilder.h"

#include "engine/AudioEngine.h"

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

void SnapshotBuilder::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
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

void SnapshotBuilder::rebuildChain(const juce::ValueTree& owner, TrackCache& cache)
{
    const auto chainNode = LiveSet::chainOf(owner);
    auto layout = std::make_shared<DeviceChainLayout>();
    std::unordered_map<std::int64_t, DevicePtr> kept;
    std::vector<DevicePtr> fresh;

    DeviceCreateContext context;
    context.tuning = &cache.processor->tuning();

    for (const auto& node : chainNode)
    {
        if (!node.hasType(ids::device))
            continue;
        const auto deviceId = LiveSet::idOf(node);
        const auto type = node[ids::deviceType].toString();
        DevicePtr device;
        if (const auto it = cache.devices.find(deviceId); it != cache.devices.end() && it->second->typeId() == type)
            device = it->second;
        if (device == nullptr || kept.contains(deviceId)) // a duplicated ID gets its own instance
        {
            device = DevicePtr(registry.create(type, context));
            applyDocument(node, *device);
            fresh.push_back(device);
        }
        kept[deviceId] = device;
        layout->devices.push_back(device);
    }
    cache.devices = std::move(kept);

    // Nothing changed for the engine (e.g. only a name): keep the published chain, so nothing fades.
    if (const auto current = cache.processor->chain();
        current != nullptr && fresh.empty() && current->devices == layout->devices)
        return;

    engine.installChain(*cache.processor, fresh, std::move(layout));
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
