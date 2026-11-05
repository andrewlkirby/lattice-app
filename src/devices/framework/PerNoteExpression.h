#pragma once

#include "core/RealtimeCheck.h"
#include "engine/MidiInputQueue.h"

#include <algorithm>
#include <array>

namespace lattice
{

/** What MPE member channels carry for their notes (TimedMidi::perNote; docs/06-m5-controllers.md,
    5g): each channel's pitch bend, pressure and timbre (CC 74), for the voices whose note
    started on that channel with the flag. An instrument keeps one; its voices read their
    channel's. Audio thread. */
struct PerNoteExpression
{
    std::array<float, 16> bend{};     // -1..1, over perNoteBendSteps
    std::array<float, 16> pressure{}; // 0..1
    std::array<float, 16> timbre{};   // 0..1

    PerNoteExpression() noexcept { reset(); }

    void reset() noexcept
    {
        bend.fill(0.0f);
        pressure.fill(0.0f);
        timbre.fill(0.5f);
    }

    /** Takes a flagged channel message (bend, pressure, CC 74): true if it was one. Notes and
        anything unflagged are the instrument's own. */
    bool handle(const TimedMidi& e) noexcept LATTICE_NONBLOCKING
    {
        if ((e.flags & TimedMidi::perNote) == 0)
            return false;
        const auto c = static_cast<std::size_t>(e.bytes[0] & 0x0f);
        switch (e.bytes[0] & 0xf0)
        {
        case 0xe0:
            bend[c] = std::clamp(static_cast<float>(((e.bytes[2] << 7) | e.bytes[1]) - 8192) / 8191.0f, -1.0f, 1.0f);
            return true;
        case 0xd0: pressure[c] = static_cast<float>(e.bytes[1]) / 127.0f; return true;
        case 0xb0:
            if (e.bytes[1] != 74)
                return false;
            timbre[c] = static_cast<float>(e.bytes[2]) / 127.0f;
            return true;
        default: return false;
        }
    }

    /** A voice's own bend on a channel, in table steps. */
    double steps(int channel) const noexcept { return static_cast<double>(bend[static_cast<std::size_t>(channel & 15)]) * perNoteBendSteps; }
    float pressureOf(int channel) const noexcept { return pressure[static_cast<std::size_t>(channel & 15)]; }
    float timbreOf(int channel) const noexcept { return timbre[static_cast<std::size_t>(channel & 15)]; }

    /** Whether a note message carries the flag (its voice then follows its channel's). */
    static bool isPerNote(const TimedMidi& e) noexcept { return (e.flags & TimedMidi::perNote) != 0; }
};

} // namespace lattice
