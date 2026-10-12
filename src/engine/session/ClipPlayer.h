#pragma once

#include "core/RealtimeCheck.h"
#include "engine/MidiInputQueue.h"
#include "engine/Transport.h"
#include "engine/session/ClipData.h"

#include <array>
#include <cstdint>
#include <limits>

namespace lattice
{

/** A track's MIDI events for one chunk, at sample offsets from the chunk's start. Fixed capacity:
    events beyond it are dropped and counted, never allocated. */
struct MidiEventSink
{
    TimedMidi* events = nullptr;
    int capacity = 0;
    int count = 0;
    std::int64_t chunkStart = 0;
    std::uint32_t dropped = 0;

    void add(std::int64_t sample, std::uint8_t status, std::uint8_t data1,
             std::uint8_t data2) noexcept LATTICE_NONBLOCKING
    {
        if (count >= capacity)
        {
            ++dropped;
            return;
        }
        auto& e = events[count++];
        e.sampleOffset = static_cast<int>(sample - chunkStart);
        e.bytes[0] = status;
        e.bytes[1] = data1;
        e.bytes[2] = data2;
        e.size = 3;
    }

    void add(std::int64_t sample, const TimedMidi& message) noexcept LATTICE_NONBLOCKING
    {
        if (count >= capacity)
        {
            ++dropped;
            return;
        }
        auto& e = events[count++];
        e = message;
        e.sampleOffset = static_cast<int>(sample - chunkStart);
    }
};

/** Plays one MIDI clip on a track (docs/03-m2-engine-session.md, "ClipPlayer"). Audio thread only;
    all state is fixed-size.

    A launch "anchors" the clip: clip position anchorPos plays at beat anchorBeat. From there the
    clip plays to its loop end, then repeats the loop region (or stops, when looping is off). Every
    event's sample is computed from the anchor and the transport, never accumulated, so the output
    is identical whatever the block sizes. Several loop wraps within one block are handled.

    The player remembers which notes it has sounding and sends their note-offs whenever the loop
    wraps, the clip stops or is replaced, or an edit removes them, so no note is ever left stuck. */
class ClipPlayer
{
public:
    static constexpr double infinity = std::numeric_limits<double>::infinity();

    enum class Status : std::uint8_t
    {
        Playing,
        Ended // a non-looping clip reached its end (its notes have been released)
    };

    bool isActive() const noexcept { return data != nullptr; }
    std::int64_t clipId() const noexcept { return id; }
    const ClipData* clip() const noexcept { return data; }
    /** Beat at which the current launch started (follow actions count from here). */
    double launchBeat() const noexcept { return launchedAt; }
    bool isOpenEnded() const noexcept { return openEnded; }
    /** The loop region in effect (the clip's, or the one set by finishOpenEnded()). */
    double effectiveLoopStart() const noexcept { return loop.start; }
    double effectiveLoopEnd() const noexcept { return loop.end; }
    bool effectiveLooping() const noexcept { return loop.looping; }

    /** Starts clip so that clip position startPosition plays at beat. With openEnded (an
        open-length recording) there is no loop end: the clip just keeps growing. */
    void start(const ClipData* clip, double beat, double startPosition,
               bool openEnded = false) noexcept LATTICE_NONBLOCKING;

    /** Releases every sounding note at sample and stops. */
    void stop(std::int64_t sample, MidiEventSink& out) noexcept LATTICE_NONBLOCKING;

    /** Emits the events whose samples fall in [from, to). Returns Ended if a non-looping clip
        reached its end; the player is then inactive. */
    Status render(const Transport& transport, std::int64_t from, std::int64_t to,
                  MidiEventSink& out) noexcept LATTICE_NONBLOCKING;

    /** Clip position at a beat (meaningful while active). */
    double positionAt(double beat) const noexcept LATTICE_NONBLOCKING;

    /** The same clip was rebuilt (edited) while playing. Playback continues from the same
        position; if the loop changed, the clip re-anchors there. Sounding notes that the edit
        removed are released at sample. Returns Ended if the new loop leaves nothing to play. */
    Status replaceClipData(const ClipData* newClip, const Transport& transport, std::int64_t sample,
                           MidiEventSink& out) noexcept LATTICE_NONBLOCKING;

    /** Ends an open-ended recording: from now on the clip loops [0, loopLength). Kept until the
        model's own loop for the clip changes (the recording's result arriving, or a user edit). */
    void finishOpenEnded(double loopLength) noexcept LATTICE_NONBLOCKING;

    int soundingCount() const noexcept { return numSounding; }

private:
    struct Loop
    {
        double start = 0.0;
        double end = 0.0;
        bool looping = true;
        bool operator==(const Loop&) const = default;
    };

    static Loop loopOf(const ClipData& clip) noexcept { return {clip.loopStart, clip.loopEnd, clip.looping}; }
    void updateIndices() noexcept;
    /** Clip time (beats since the anchor) at which iteration n starts. */
    double iterationStart(int n) const noexcept;
    double eventTime(std::size_t index) const noexcept;
    void releaseAll(std::int64_t sample, MidiEventSink& out) noexcept;
    void seekTo(const Transport& transport, std::int64_t sample) noexcept;

    const ClipData* data = nullptr;
    std::int64_t id = 0;
    Loop loop;
    bool openEnded = false;
    bool overrideActive = false;
    Loop clipLoopAtOverride; // the clip's own loop when finishOpenEnded() was called

    double anchorBeat = 0.0;
    double anchorPos = 0.0;
    double launchedAt = 0.0;
    int iteration = 0;
    std::size_t next = 0;
    std::size_t firstPassBegin = 0; // first event of iteration 0
    std::size_t loopBegin = 0;      // first event of later iterations
    std::size_t endIndex = 0;       // events at or past the loop end are never played

    std::array<bool, 128> sounding{};
    int numSounding = 0;
};

} // namespace lattice
