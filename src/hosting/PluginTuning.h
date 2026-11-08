#pragma once

#include "tuning/external/MtsEsp.h"

#include <juce_events/juce_events.h>

#include <map>

namespace lattice
{
class SessionController;
class TrackTuning;
}

namespace lattice::hosting
{

class PluginHost;

/** Plugins playing each track's tuning through MTS-ESP (docs/07-m6-plugins-external.md, 6c).

    Each tick it takes the tracks with a plugin instrument loaded and the tuning each plays: what the
    track's own instruments play, 128 notes at the track's note offset, with what its high-note
    policy mutes at the current sample rate filtered (TrackTuning::hzFor()). It sends those to the
    master, and gives each plugin the channel its track's tuning got (or none). Nothing is worked
    out again for a track whose tuning, offset, policy and sample rate haven't changed, so a tick
    costs a walk over the plugins. Every two seconds it asks the master to try again if it isn't one
    (MTS-ESP installed since, or another master gone).

    Plugin effects and MIDI effects don't play notes: they're left as they are. Message thread. */
class PluginTuning final : private juce::Timer
{
public:
    PluginTuning(SessionController& session, PluginHost& plugins, mts::Master& master);
    ~PluginTuning() override;

    /** What the timer does (tests call it). */
    void update();

    /** The tuning a track's plugins play, as sent (128 notes; 0 for filtered ones). */
    static mts::Tuning tuningOf(const TrackTuning& tuning);

private:
    void timerCallback() override;

    struct Track
    {
        const void* table = nullptr;
        int offset = 0, policy = 0;
        double sampleRate = 0.0;
        mts::Tuning tuning;
    };

    SessionController& session;
    PluginHost& plugins;
    mts::Master& master;
    std::map<std::int64_t, Track> tracks; // by track ID
    int ticks = 0;
};

} // namespace lattice::hosting
