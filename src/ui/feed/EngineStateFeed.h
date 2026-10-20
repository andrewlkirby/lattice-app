#pragma once

#include "core/AudioCallbackStats.h"
#include "engine/SessionController.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>
#include <memory>
#include <utility>
#include <vector>

namespace lattice::ui
{

/** The UI's one view of the engine's live state (docs/05-m4-ui.md, "Rules", 1 and 3): the
    transport, every slot's state, each track's play state and last note, the meters, and the
    audio callback's load. It's read once per display refresh (VBlankAttachment), and only the
    listeners interested in what changed are told, so there are no per-component timers and only
    the parts that changed repaint.

    Peaks are taken from the engine once per tick (the loudest since the last tick), so nothing is
    missed between frames and two views showing one meter see the same level. Message thread only. */
class EngineStateFeed
{
public:
    enum Aspect : std::uint32_t
    {
        transportAspect = 1u << 0, // playing, position, tempo, time signature, session record
        slotsAspect = 1u << 1,     // any slot's state, any track's play state (clip progress)
        metersAspect = 1u << 2,    // track, return and master levels (every tick while any is moving)
        notesAspect = 1u << 3,     // a track's last note
        loadAspect = 1u << 4,      // the audio callback's load and dropouts (every loadIntervalSeconds)
        allAspects = 0xffffffffu
    };

    struct Listener
    {
        virtual ~Listener() = default;
        /** aspects: the bits of what changed this tick (only those this listener asked for). */
        virtual void engineStateChanged(std::uint32_t aspects) = 0;
    };

    /** Read four times a second, not every frame: the percentiles take a pass over a histogram,
        and a load figure that changes 60 times a second can't be read anyway. */
    struct Load
    {
        AudioCallbackStats::Summary callback{};
        int deviceXruns = 0;
    };
    static constexpr double loadIntervalSeconds = 0.25;

    EngineStateFeed(SessionController& session, AudioEngine& engine);
    ~EngineStateFeed();

    void addListener(Listener* listener, std::uint32_t aspects);
    void removeListener(Listener* listener);

    /** Ticks once per display refresh of the component's window (until detached). */
    void attachTo(juce::Component& component);
    void detach();

    /** Reads the engine and tells the listeners what changed. nowSeconds: a monotonic clock. */
    void tick(double nowSeconds);
    /** The time since the tick before (0 on the first): meters and blinking advance by it. */
    double frameSeconds() const noexcept { return lastFrameSeconds; }
    std::uint64_t ticks() const noexcept { return tickCount; }
    /** The time the ticks took since the last call (the shell adds it to the frame's work). */
    double takeTickWorkSeconds() noexcept { return std::exchange(tickWork, 0.0); }

    // --- What the last tick read ---------------------------------------------------------------
    const TransportState& transport() const noexcept { return transportState; }
    SlotState slot(int track, int scene) const;
    const TrackPlayState& trackPlay(int track) const;
    int lastNote(int track) const;
    /** Linear peaks since the tick before (left, right). */
    std::array<float, 2> trackPeaks(int track) const;
    std::array<float, 2> returnPeaks(int returnIndex) const;
    std::array<float, 2> masterPeaks() const noexcept { return master; }
    const Load& load() const noexcept { return loadState; }
    /** The slots whose state changed in the last tick (track, scene). */
    const std::vector<std::pair<int, int>>& changedSlots() const noexcept { return slotChanges; }

private:
    void tickBody(double nowSeconds);

    struct Track
    {
        std::vector<SlotState> slots;
        TrackPlayState play{};
        std::array<float, 2> peaks{};
        int lastNote = -1;
    };
    struct Subscriber
    {
        Listener* listener;
        std::uint32_t aspects;
    };

    SessionController& session;
    AudioEngine& engine;
    std::unique_ptr<juce::VBlankAttachment> vblank;
    std::vector<Subscriber> subscribers;

    TransportState transportState{};
    std::vector<Track> tracks;
    std::vector<std::array<float, 2>> returns;
    std::array<float, 2> master{};
    Load loadState{};
    std::vector<std::pair<int, int>> slotChanges;
    double lastTickSeconds = -1.0;
    double lastLoadSeconds = -1.0e9;
    double lastFrameSeconds = 0.0;
    std::uint64_t tickCount = 0;
    double tickWork = 0.0;
    bool metersWereMoving = false;
};

} // namespace lattice::ui
