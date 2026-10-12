#pragma once

#include "core/model/SessionTypes.h"
#include "engine/TrackProcessor.h"
#include "engine/session/ClipData.h"

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
    bool mute = false;
    bool solo = false;
    bool arm = false;
    std::vector<SlotSnapshot> slots; // one per scene

    /** Index of the slot holding a clip, or -1. */
    int sceneOfClip(std::int64_t clipId) const noexcept
    {
        for (std::size_t i = 0; i < slots.size(); ++i)
            if (slots[i].clip != nullptr && slots[i].clip->id == clipId)
                return static_cast<int>(i);
        return -1;
    }
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
    bool anySolo = false;
    std::vector<TrackSnapshot> tracks;
    std::vector<SceneSnapshot> scenes;

    const TrackSnapshot* findTrack(std::int64_t trackId) const noexcept
    {
        for (const auto& t : tracks)
            if (t.id == trackId)
                return &t;
        return nullptr;
    }
};

/** Constant-power pan law (−3 dB at the centre): pan −1..1 → {left, right} gains. */
inline std::pair<float, float> constantPowerPan(double pan) noexcept
{
    const double angle = (std::clamp(pan, -1.0, 1.0) + 1.0) * 0.25 * 3.14159265358979323846;
    return {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
}

} // namespace lattice
