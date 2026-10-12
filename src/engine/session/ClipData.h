#pragma once

#include "core/RealtimeCheck.h"
#include "core/model/SessionTypes.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace lattice
{

/** One note-on or note-off at a clip position (beats from the clip's start). */
struct ClipEvent
{
    double position = 0.0;
    std::uint8_t pitch = 0;
    std::uint8_t velocity = 0; // 0 for note-off
    bool isNoteOn() const noexcept { return velocity > 0; }
};

/** A note after clean-up: overlapping notes of the same pitch are cut where the next one starts. */
struct ClipNoteSpan
{
    double start = 0.0;
    double end = 0.0;
    std::uint8_t pitch = 0;
};

/** Immutable playback form of a clip (docs/03-m2-engine-session.md, "EngineSnapshot"), built on
    the message thread from the model and read by the audio thread.

    - Muted notes are left out.
    - Overlapping notes of the same pitch are cut at the later note's start, as in Live, so a
      pitch's note-ons and note-offs always alternate.
    - events holds every note-on and note-off sorted by position, note-offs first at equal
      positions. Notes running past the loop end are cut by the player when it wraps or stops, so
      their note-offs past the end are never reached. */
struct ClipData
{
    std::int64_t id = 0;
    double startMarker = 0.0;
    double loopStart = 0.0;
    double loopEnd = 4.0;
    bool looping = true;
    LaunchMode launchMode = LaunchMode::Trigger;
    Quantization quantization = Quantization::Global;
    bool legato = false;
    FollowAction followA = FollowAction::None;
    FollowAction followB = FollowAction::None;
    double followChanceA = 1.0; // 0..1
    double followTime = 4.0;    // beats after the launch

    std::vector<ClipEvent> events;
    std::vector<ClipNoteSpan> notes; // sorted by start

    double loopLength() const noexcept { return loopEnd - loopStart; }
    bool hasFollowActions() const noexcept
    {
        return (followA != FollowAction::None || followB != FollowAction::None) && followTime > 0.0;
    }

    /** Index of the first event at or after position. */
    std::size_t firstEventAtOrAfter(double position) const noexcept LATTICE_NONBLOCKING;

    /** True if a note of this pitch covers position (start <= position < end). */
    bool noteCovers(int pitch, double position) const noexcept LATTICE_NONBLOCKING;

    /** Builds from a CLIP tree. Loop values are sanitised: the loop is at least
        LiveSet::minLoopLength long and the start marker lies before the loop end. */
    static std::shared_ptr<const ClipData> fromTree(const juce::ValueTree& clip);
};

using ClipDataPtr = std::shared_ptr<const ClipData>;

} // namespace lattice
