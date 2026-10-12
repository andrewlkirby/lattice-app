#pragma once

#include "core/model/LiveSet.h"
#include "engine/EngineSnapshot.h"
#include "tuning/TuningTable.h"

#include <juce_events/juce_events.h>

#include <functional>
#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace lattice
{

class AudioEngine;

/** Turns the document into EngineSnapshots and publishes them to the engine
    (docs/03-m2-engine-session.md, "EngineSnapshot"). Message thread only.

    - Listens to the ValueTree; any change marks the snapshot dirty, and a change inside a clip
      marks only that clip for rebuilding. Unchanged clips are shared with the previous snapshot.
    - Rebuilds are coalesced (one per message-loop turn). rebuildIfNeeded() forces a pending rebuild
      now; the session controller calls it before every engine command, so commands always see the
      document as it is.
    - Keeps one TrackProcessor per track ID across rebuilds. */
class SnapshotBuilder final : private juce::ValueTree::Listener, private juce::AsyncUpdater
{
public:
    using TuningResolver = std::function<TuningPtr(const juce::String& tuningId)>;

    SnapshotBuilder(LiveSet& set, AudioEngine& engine, TuningResolver resolveTuning);
    ~SnapshotBuilder() override;

    void rebuildIfNeeded();
    void rebuildNow();
    /** Forces a rebuild on the next turn (e.g. a tuning file was loaded that tracks may refer to). */
    void invalidate() { markDirty(set.state()); }
    bool isDirty() const noexcept { return dirty; }

    /** Drops every track processor (and cached clip), so the next rebuild starts fresh. Used when a
        different set is loaded: IDs restart in every set, so they can't identify tracks across sets. */
    void forgetAll();

    /** The processor of a track, or nullptr if the track is not in the last snapshot. */
    std::shared_ptr<TrackProcessor> processorFor(std::int64_t trackId) const;

    /** Frees snapshots and tunings the audio thread has finished with. Call regularly. */
    void collectGarbage();

    std::uint64_t generation() const noexcept { return lastGeneration; }
    /** How many clips the last rebuild had to build (the rest were reused). For tests. */
    int clipsBuiltInLastRebuild() const noexcept { return lastClipsBuilt; }

private:
    void markDirty(const juce::ValueTree& changed);
    void markClipsIn(const juce::ValueTree& tree);
    void handleAsyncUpdate() override;

    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier&) override
    {
        markDirty(tree);
    }
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int) override { markDirty(parent); }
    void valueTreeChildOrderChanged(juce::ValueTree& parent, int, int) override { markDirty(parent); }
    void valueTreeRedirected(juce::ValueTree&) override;

    struct TrackCache
    {
        std::shared_ptr<TrackProcessor> processor;
        juce::String tuningId;
        TuningPtr tuning;
    };

    LiveSet& set;
    AudioEngine& engine;
    TuningResolver resolveTuning;
    bool dirty = true;
    std::unordered_map<std::int64_t, ClipDataPtr> clipCache;
    std::unordered_set<std::int64_t> dirtyClips;
    std::unordered_map<std::int64_t, TrackCache> tracks;
    std::uint64_t lastGeneration = 0;
    int lastClipsBuilt = 0;
};

} // namespace lattice
