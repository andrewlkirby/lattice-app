#pragma once

#include "core/model/SessionTypes.h"
#include "engine/TrackProcessor.h"
#include "engine/session/ClipData.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace lattice
{

struct SlotSnapshot
{
    ClipDataPtr clip; // null for an empty slot
    bool hasStopButton = true;
};

struct TrackSnapshot
{
    std::int64_t id = 0;
    std::shared_ptr<TrackProcessor> processor;
    float gainLeft = 0.0f; // volume × constant-power pan, before mute/solo
    float gainRight = 0.0f;
    /** Muted itself or by a group it's in (a muted group silences everything in it). */
    bool mute = false;
    /** On a soloed path: soloed itself, or a group it's in, or something in it (so a soloed
        track's groups stay audible). */
    bool solo = false;
    bool arm = false;
    /** Live input: from which source (-1 every one; computerKeyboardSource or an input's number),
        on which channel (0 every one), and when it's heard. */
    int inputSource = -1;
    int inputChannel = 0;
    Monitoring monitoring = Monitoring::Auto;
    bool isGroup = false; // a group track: its chain's input is its members' mix
    int parent = -1;      // the index of the group track it mixes into, or -1 for the master
    /** Samples its sends wait (TrackProcessor::Sends::delay): the latency of its groups' chains. */
    int sendDelay = 0;
    std::vector<SlotSnapshot> slots; // one per scene
    std::array<float, TrackProcessor::maxReturns> sendGains{}; // per return, linear (0 = off)
    int compensation = 0; // samples of delay that line this track up with the slowest

    /** Index of the slot holding a clip, or -1. */
    int sceneOfClip(std::int64_t clipId) const noexcept
    {
        for (std::size_t i = 0; i < slots.size(); ++i)
            if (slots[i].clip != nullptr && slots[i].clip->id == clipId)
                return static_cast<int>(i);
        return -1;
    }
};

/** A return track: its chain is fed by the tracks' sends (docs/04-m3-devices.md, "3e"). */
struct ReturnSnapshot
{
    std::int64_t id = 0;
    std::shared_ptr<TrackProcessor> processor;
    float gainLeft = 0.0f;
    float gainRight = 0.0f;
    bool mute = false;
    bool solo = false;
    bool preFader = false;
    int compensation = 0;
};

struct SceneSnapshot
{
    double tempo = 0.0; // 0 = none
    int timeSigNum = 0; // 0 = none
    int timeSigDen = 0;
};

/** Everything the audio thread needs from the document, immutable once published
    (docs/03-m2-engine-session.md, "EngineSnapshot"). Built by SnapshotBuilder; read through
    RtShared by raw pointer. Unchanged clips and track processors are shared between snapshots. */
struct EngineSnapshot
{
    std::uint64_t generation = 0;
    double tempo = 120.0;
    int timeSigNum = 4;
    int timeSigDen = 4;
    Quantization globalQuantization = Quantization::Bar1;
    bool metronomeOn = false;
    float metronomeGain = 0.25f;
    float masterGainLeft = 0.70710678f;
    float masterGainRight = 0.70710678f;
    bool anySolo = false;       // a track is soloed
    bool anyReturnSolo = false; // a return is soloed
    std::vector<TrackSnapshot> tracks;
    std::vector<SceneSnapshot> scenes;
    std::vector<ReturnSnapshot> returns;
    std::shared_ptr<TrackProcessor> master; // the master's device chain
    /** Track indices in processing order, in levels: every sidechain source is in an earlier level
        than the tracks that listen to it, and the tracks within a level are independent (they can
        run in parallel). Group tracks aren't in them (see groupLevels). */
    std::vector<std::vector<int>> levels;
    /** Group track indices in levels, deepest first: a group renders after everything in it has
        been mixed into its input, and the groups of one level are independent. */
    std::vector<std::vector<int>> groupLevels;
    /** The tracks' direct path to the master is delayed by the slowest return's latency, so it
        lines up with what comes back through the returns. */
    int masterInputDelay = 0;
    bool reducedLatencyWhenMonitoring = false;

    const TrackSnapshot* findTrack(std::int64_t trackId) const noexcept
    {
        for (const auto& t : tracks)
            if (t.id == trackId)
                return &t;
        return nullptr;
    }
};

/** Balance (for a stereo signal, a group track's): unity at the centre; turning towards one side
    turns the other side down, to silence at the end. pan −1..1 → {left, right} gains. */
inline std::pair<float, float> balancePan(double pan) noexcept
{
    const double p = std::clamp(pan, -1.0, 1.0);
    return {static_cast<float>(p > 0.0 ? 1.0 - p : 1.0), static_cast<float>(p < 0.0 ? 1.0 + p : 1.0)};
}

/** Constant-power pan law (−3 dB at the centre): pan −1..1 → {left, right} gains. */
inline std::pair<float, float> constantPowerPan(double pan) noexcept
{
    const double angle = (std::clamp(pan, -1.0, 1.0) + 1.0) * 0.25 * 3.14159265358979323846;
    return {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
}

} // namespace lattice
