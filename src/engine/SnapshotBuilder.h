#pragma once

#include "core/model/LiveSet.h"
#include "devices/framework/DeviceRegistry.h"
#include "devices/racks/RackLayout.h"
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
    - Keeps one TrackProcessor per track ID across rebuilds.
    - Builds each track's device chain (docs/04-m3-devices.md, "Chain changes"): device instances
      are kept per device ID, so devices that stay in a chain keep their sound; new ones are created
      from the registry, given their saved values and prepared before the chain is published.
    - Parameter values, the on/off switch and a device's STATE go straight to the device instance,
      without rebuilding anything, so turning a knob costs no more than storing a value. */
class SnapshotBuilder final : private juce::ValueTree::Listener, private juce::AsyncUpdater
{
public:
    using TuningResolver = std::function<TuningPtr(const juce::String& tuningId)>;

    SnapshotBuilder(LiveSet& set, AudioEngine& engine, TuningResolver resolveTuning,
                    const DeviceRegistry& registry = DeviceRegistry::builtin());
    ~SnapshotBuilder() override;

    void rebuildIfNeeded();
    /** The track auto-armed by selection (0: none): it's armed in the snapshot while no track is
        armed in the document. Not part of the document (no undo step, nothing to save). */
    void setAutoArmTrack(std::int64_t trackId);
    std::int64_t autoArmTrack() const noexcept { return autoArm; }
    void rebuildNow();
    /** Forces a rebuild on the next turn (e.g. a tuning file was loaded that tracks may refer to). */
    void invalidate() { markDirty(set.state()); }
    bool isDirty() const noexcept { return dirty; }

    /** Drops every track processor (and cached clip), so the next rebuild starts fresh. Used when a
        different set is loaded: IDs restart in every set, so they can't identify tracks across sets. */
    void forgetAll();

    /** The processor of a track, or nullptr if the track is not in the last snapshot. */
    std::shared_ptr<TrackProcessor> processorFor(std::int64_t trackId) const;

    /** The instance of a device (by its document ID) in the last rebuild, or nullptr. */
    StudioDevice* deviceFor(std::int64_t deviceId) const;
    const DeviceRegistry& devices() const noexcept { return registry; }

    /** Frees snapshots, chains and tunings the audio thread has finished with. Call regularly. */
    void collectGarbage();

    std::uint64_t generation() const noexcept { return lastGeneration; }
    /** How many clips the last rebuild had to build (the rest were reused). For tests. */
    int clipsBuiltInLastRebuild() const noexcept { return lastClipsBuilt; }

private:
    void markDirty(const juce::ValueTree& changed);
    void markClipsIn(const juce::ValueTree& tree);
    void handleAsyncUpdate() override;

    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override;
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child) override;
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged(juce::ValueTree& parent, int, int) override { markDirty(parent); }
    void valueTreeRedirected(juce::ValueTree&) override;

    struct TrackCache
    {
        std::shared_ptr<TrackProcessor> processor;
        juce::String tuningId;
        TuningPtr tuning;
        std::unordered_map<std::int64_t, DevicePtr> devices; // by device ID (racks' contents too)
        std::unordered_map<std::int64_t, std::shared_ptr<RackChainState>> rackChains; // by chain ID
    };

    /** What a chain rebuild collects: what is new, for the engine to prepare. */
    struct ChainBuild
    {
        TrackCache* cache = nullptr;
        std::unordered_map<std::int64_t, DevicePtr> kept;
        std::unordered_map<std::int64_t, std::shared_ptr<RackChainState>> keptChains;
        std::vector<DevicePtr> fresh;
        std::vector<std::shared_ptr<RackChainState>> freshChains;
        std::vector<SidechainRoute> sidechainSources;
        DeviceCreateContext context;
    };
    /** One DEVICES node to a layout (racks inside, up to RackDevice::maxDepth deep). */
    void buildChain(const juce::ValueTree& devicesNode, DeviceChainLayout& out, ChainBuild& build, int depth);
    std::shared_ptr<RackLayout> buildRack(const juce::ValueTree& rackNode, ChainBuild& build, int depth);

    /** The STATE node that `node` is in (or is), if it belongs to a device. */
    static juce::ValueTree enclosingDeviceState(const juce::ValueTree& node);
    void forwardParameter(const juce::ValueTree& values, const juce::Identifier& parameterId);
    void forwardState(const juce::ValueTree& stateNode);
    /** Copies the document's values for a device into its instance. */
    static void applyDocument(const juce::ValueTree& deviceNode, StudioDevice& device);
    static void applyChainMix(const juce::ValueTree& chainNode, RackChainState& state);
    void rebuildChain(const juce::ValueTree& owner, TrackCache& cache);

    LiveSet& set;
    AudioEngine& engine;
    TuningResolver resolveTuning;
    const DeviceRegistry& registry;
    bool dirty = true;
    std::int64_t autoArm = 0;
    bool allChainsDirty = true;
    std::unordered_set<std::int64_t> dirtyChains; // IDs of chain owners (tracks)
    std::unordered_map<std::int64_t, StudioDevice*> deviceIndex;
    /** Each chain owner's sidechain sources (by owner ID), from its last chain build. */
    std::unordered_map<std::int64_t, std::vector<SidechainRoute>> trackSidechains;
    std::unordered_map<std::int64_t, ClipDataPtr> clipCache;
    std::unordered_set<std::int64_t> dirtyClips;
    std::unordered_map<std::int64_t, TrackCache> tracks;
    std::uint64_t lastGeneration = 0;
    int lastClipsBuilt = 0;
};

} // namespace lattice
