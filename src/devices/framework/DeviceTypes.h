#pragma once

#include "core/RealtimeCheck.h"
#include "engine/MidiInputQueue.h"

#include <cstdint>

namespace lattice
{

class TrackTuning;
class SamplePool;

enum class DeviceKind : std::uint8_t
{
    MidiEffect,
    Instrument,
    AudioEffect,
    Rack // an Instrument or Effect Rack; it says itself which one it acts as
};

/** MIDI events for one block, at sample offsets within it, in non-decreasing time order. Fixed
    capacity: events beyond it are dropped and counted, never allocated. */
struct MidiBlock
{
    TimedMidi* events = nullptr;
    int capacity = 0;
    int count = 0;
    std::uint32_t dropped = 0;

    void clear() noexcept { count = 0; }

    void add(int offset, std::uint8_t status, std::uint8_t data1, std::uint8_t data2) noexcept LATTICE_NONBLOCKING
    {
        if (count >= capacity)
        {
            ++dropped;
            return;
        }
        auto& e = events[count++];
        e.sampleOffset = offset;
        e.bytes[0] = status;
        e.bytes[1] = data1;
        e.bytes[2] = data2;
        e.size = 3;
    }

    void add(const TimedMidi& e) noexcept LATTICE_NONBLOCKING
    {
        if (count >= capacity)
        {
            ++dropped;
            return;
        }
        events[count++] = e;
    }

    /** Inserts keeping time order (after events at the same offset). For MIDI effects that make
        events out of order. */
    void insertSorted(const TimedMidi& e) noexcept LATTICE_NONBLOCKING
    {
        if (count >= capacity)
        {
            ++dropped;
            return;
        }
        int i = count;
        while (i > 0 && events[i - 1].sampleOffset > e.sampleOffset)
        {
            events[i] = events[i - 1];
            --i;
        }
        events[i] = e;
        ++count;
    }

    const TimedMidi* begin() const noexcept { return events; }
    const TimedMidi* end() const noexcept { return events + count; }
};

inline bool isNoteOn(const TimedMidi& e) noexcept
{
    return (e.bytes[0] & 0xf0) == 0x90 && e.bytes[2] > 0;
}

inline bool isNoteOff(const TimedMidi& e) noexcept
{
    return (e.bytes[0] & 0xf0) == 0x80 || ((e.bytes[0] & 0xf0) == 0x90 && e.bytes[2] == 0);
}

/** Musical time for one block, as devices see it. While the transport is stopped, beatAtStart is
    a free-running beat at the current tempo, so tempo-synced devices (LFOs, delays, the
    arpeggiator) keep working. */
struct DeviceTimeInfo
{
    double sampleRate = 48000.0;
    double tempo = 120.0;
    double samplesPerBeat = 24000.0;
    double beatAtStart = 0.0;
    std::int64_t blockStartSample = 0;
    bool playing = false;

    double beatAt(int offset) const noexcept { return beatAtStart + offset / samplesPerBeat; }
};

/** What a device processes in one block: stereo audio in place, MIDI in, and (MIDI effects) MIDI
    out. Instruments overwrite the audio; audio effects transform it. */
struct DeviceBuffers
{
    float* const* channels = nullptr;
    int numChannels = 2;
    int numSamples = 0;
    MidiBlock* midiIn = nullptr;
    MidiBlock* midiOut = nullptr; // MIDI effects only
    const float* const* sidechain = nullptr; // a sidechain source's audio, or nullptr (3e)
};

/** What a device gets from its track when it is created. */
struct DeviceCreateContext
{
    const TrackTuning* tuning = nullptr; // the track's tuning; outlives the device
    SamplePool* samples = nullptr;       // the shared sample pool (3b)
};

} // namespace lattice
