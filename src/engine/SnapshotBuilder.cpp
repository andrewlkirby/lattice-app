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

SnapshotBuilder::SnapshotBuilder(LiveSet& s, AudioEngine& e, TuningResolver resolver)
    : set(s), engine(e), resolveTuning(std::move(resolver))
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

void SnapshotBuilder::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child)
{
    // A subtree that arrives whole (undo, paste, a loaded set) may reuse IDs of cached clips.
    markClipsIn(child);
    markDirty(parent);
}

void SnapshotBuilder::valueTreeRedirected(juce::ValueTree&)
{
    clipCache.clear();
    markDirty(set.state());
}

void SnapshotBuilder::forgetAll()
{
    tracks.clear();
    clipCache.clear();
    dirtyClips.clear();
    markDirty(set.state());
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

        auto cache = tracks.contains(ts.id) ? tracks[ts.id] : TrackCache{};
        if (cache.processor == nullptr)
            cache.processor = engine.createTrackProcessor(ts.id);

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

        newTracks[ts.id] = cache;
        snapshot->tracks.push_back(std::move(ts));
    }

    clipCache = std::move(newClipCache);
    dirtyClips.clear();
    tracks = std::move(newTracks);
    engine.publishSnapshot(std::move(snapshot));
}

void SnapshotBuilder::collectGarbage()
{
    engine.collectGarbage();
    for (auto& [id, cache] : tracks)
        cache.processor->tuning().collectGarbage();
}

} // namespace lattice
