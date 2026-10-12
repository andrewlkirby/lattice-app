#pragma once

#include <cstdint>

namespace lattice
{

/** Message thread (or a controller thread) -> audio thread. Plain data, sent through an MpscQueue. */
enum class CommandType : std::uint8_t
{
    LaunchSlot,       // trackId, scene, clipId (0 for an empty slot: its stop button)
    ReleaseSlot,      // trackId, scene, clipId: launch button released (Gate mode)
    LaunchScene,      // scene
    StopTrack,        // trackId
    StopAll,          // a second press while the first is still queued stops at once
    Play,             // from bar 1
    Continue,         // from the stopped position
    Stop,             // a second press while stopped rewinds to bar 1
    StartRecording,   // trackId, scene, clipId (created by the controller), flag = open-ended
    EndRecording,     // trackId: ends an open-ended recording at the launch quantization
    SetSessionRecord, // flag
    SetRandomSeed     // value (follow actions)
};

struct EngineCommand
{
    CommandType type = CommandType::Play;
    bool flag = false;
    std::int32_t scene = -1;
    std::int64_t trackId = 0;
    std::int64_t clipId = 0;
    std::uint64_t value = 0;
};

/** Audio thread -> message thread, through an SpscFifo. */
enum class EngineEventType : std::uint8_t
{
    CaptureNoteOn,       // trackId, clipId, pitch, velocity, position (clip position), beat
    CaptureNoteOff,      // trackId, clipId, pitch, beat
    CaptureEnded,        // trackId, clipId, value = final loop length for a new recording (0 for overdub)
    RecordingCancelled,  // trackId, clipId: a queued recording was replaced before it began
    TempoChanged,        // value = BPM (a scene's tempo took effect)
    TimeSignatureChanged // numerator, denominator (a scene's time signature took effect)
};

struct EngineEvent
{
    EngineEventType type = EngineEventType::TempoChanged;
    std::uint8_t pitch = 0;
    std::uint8_t velocity = 0;
    std::int16_t numerator = 0;
    std::int16_t denominator = 0;
    std::int64_t trackId = 0;
    std::int64_t clipId = 0;
    double position = 0.0;
    double beat = 0.0;
    double value = 0.0;
};

} // namespace lattice
