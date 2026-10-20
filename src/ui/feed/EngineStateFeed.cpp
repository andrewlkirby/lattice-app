#include "ui/feed/EngineStateFeed.h"

#include <algorithm>

namespace lattice::ui
{
namespace
{
bool sameTransport(const TransportState& a, const TransportState& b)
{
    return a.playing == b.playing && a.sessionRecord == b.sessionRecord && a.stopAllQueued == b.stopAllQueued &&
           a.timeSigNum == b.timeSigNum && a.timeSigDen == b.timeSigDen && a.beat == b.beat && a.tempo == b.tempo;
}

bool samePlay(const TrackPlayState& a, const TrackPlayState& b)
{
    return a.playingClipId == b.playingClipId && a.queuedClipId == b.queuedClipId && a.queuedScene == b.queuedScene &&
           a.queued == b.queued && a.recording == b.recording && a.position == b.position && a.loopStart == b.loopStart &&
           a.loopEnd == b.loopEnd;
}

bool moving(const std::array<float, 2>& peaks)
{
    return peaks[0] > 0.0f || peaks[1] > 0.0f;
}

const TrackPlayState noPlay{};
} // namespace

EngineStateFeed::EngineStateFeed(SessionController& s, AudioEngine& e) : session(s), engine(e)
{
}

EngineStateFeed::~EngineStateFeed()
{
    detach();
}

void EngineStateFeed::addListener(Listener* listener, std::uint32_t aspects)
{
    removeListener(listener);
    subscribers.push_back({listener, aspects});
}

void EngineStateFeed::removeListener(Listener* listener)
{
    std::erase_if(subscribers, [listener](const Subscriber& s) { return s.listener == listener; });
}

void EngineStateFeed::attachTo(juce::Component& component)
{
    vblank = std::make_unique<juce::VBlankAttachment>(&component, [this](double timestampSec) { tick(timestampSec); });
}

void EngineStateFeed::detach()
{
    vblank.reset();
}

SlotState EngineStateFeed::slot(int track, int scene) const
{
    if (track < 0 || track >= static_cast<int>(tracks.size()))
        return SlotState::Empty;
    const auto& slots = tracks[static_cast<std::size_t>(track)].slots;
    return scene >= 0 && scene < static_cast<int>(slots.size()) ? slots[static_cast<std::size_t>(scene)] : SlotState::Empty;
}

const TrackPlayState& EngineStateFeed::trackPlay(int track) const
{
    return track >= 0 && track < static_cast<int>(tracks.size()) ? tracks[static_cast<std::size_t>(track)].play : noPlay;
}

int EngineStateFeed::lastNote(int track) const
{
    return track >= 0 && track < static_cast<int>(tracks.size()) ? tracks[static_cast<std::size_t>(track)].lastNote : -1;
}

std::array<float, 2> EngineStateFeed::trackPeaks(int track) const
{
    return track >= 0 && track < static_cast<int>(tracks.size()) ? tracks[static_cast<std::size_t>(track)].peaks
                                                                  : std::array<float, 2>{};
}

std::array<float, 2> EngineStateFeed::returnPeaks(int returnIndex) const
{
    return returnIndex >= 0 && returnIndex < static_cast<int>(returns.size()) ? returns[static_cast<std::size_t>(returnIndex)]
                                                                               : std::array<float, 2>{};
}

void EngineStateFeed::tick(double nowSeconds)
{
    const auto started = juce::Time::getHighResolutionTicks();
    tickBody(nowSeconds);
    tickWork += juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - started);
}

void EngineStateFeed::tickBody(double nowSeconds)
{
    lastFrameSeconds = lastTickSeconds < 0.0 ? 0.0 : juce::jmax(0.0, nowSeconds - lastTickSeconds);
    lastTickSeconds = nowSeconds;
    ++tickCount;
    std::uint32_t changed = 0;

    const auto t = session.transport();
    if (!sameTransport(t, transportState) || tickCount == 1)
        changed |= transportAspect;
    transportState = t;

    // Slots and play states. A different number of tracks or scenes counts as a change of all.
    const auto& set = session.set();
    const int numTracks = set.numTracks(), numScenes = set.numScenes();
    slotChanges.clear();
    if (static_cast<int>(tracks.size()) != numTracks)
    {
        tracks.resize(static_cast<std::size_t>(numTracks));
        changed |= slotsAspect | notesAspect | metersAspect;
    }
    bool anyMeterMoving = false;
    for (int ti = 0; ti < numTracks; ++ti)
    {
        auto& track = tracks[static_cast<std::size_t>(ti)];
        if (static_cast<int>(track.slots.size()) != numScenes)
        {
            track.slots.assign(static_cast<std::size_t>(numScenes), SlotState::Empty);
            changed |= slotsAspect;
        }
        for (int s = 0; s < numScenes; ++s)
        {
            const auto state = session.slotState(ti, s);
            if (state != track.slots[static_cast<std::size_t>(s)])
            {
                track.slots[static_cast<std::size_t>(s)] = state;
                slotChanges.emplace_back(ti, s);
                changed |= slotsAspect;
            }
        }
        const auto play = session.trackState(ti);
        if (!samePlay(play, track.play))
        {
            track.play = play;
            changed |= slotsAspect;
        }
        const int note = session.lastNoteOn(ti);
        if (note != track.lastNote)
        {
            track.lastNote = note;
            changed |= notesAspect;
        }
        track.peaks = session.takeTrackPeaks(ti);
        anyMeterMoving = anyMeterMoving || moving(track.peaks);
    }

    const int numReturns = set.numReturns();
    if (static_cast<int>(returns.size()) != numReturns)
    {
        returns.resize(static_cast<std::size_t>(numReturns));
        changed |= metersAspect;
    }
    for (int r = 0; r < numReturns; ++r)
    {
        returns[static_cast<std::size_t>(r)] = session.takeReturnPeaks(r);
        anyMeterMoving = anyMeterMoving || moving(returns[static_cast<std::size_t>(r)]);
    }
    master = session.takeMasterPeaks();
    anyMeterMoving = anyMeterMoving || moving(master);
    // Meters change every tick while anything sounds, and once more when it all goes quiet (so
    // they can fall to silence).
    if (anyMeterMoving || metersWereMoving)
        changed |= metersAspect;
    metersWereMoving = anyMeterMoving;

    if (nowSeconds - lastLoadSeconds >= loadIntervalSeconds)
    {
        lastLoadSeconds = nowSeconds;
        Load l;
        l.callback = engine.callbackStats().summary();
        l.deviceXruns = engine.getDeviceManager().getXRunCount();
        // Every interval, changed or not: what's shown with it (the UI's frame times) moves anyway.
        changed |= loadAspect;
        loadState = l;
    }

    if (changed == 0)
        return;
    // A listener may add or remove listeners while being told: work from a copy, and skip any that
    // went away meanwhile.
    const auto toTell = subscribers;
    for (const auto& s : toTell)
    {
        const auto aspects = s.aspects & changed;
        if (aspects == 0)
            continue;
        const bool stillThere = std::any_of(subscribers.begin(), subscribers.end(),
                                            [&](const Subscriber& x) { return x.listener == s.listener; });
        if (stillThere)
            s.listener->engineStateChanged(aspects);
    }
}

} // namespace lattice::ui
