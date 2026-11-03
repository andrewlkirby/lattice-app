#pragma once

#include "core/RealtimeCheck.h"
#include "core/RtFifo.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <cstdint>

namespace lattice
{

/** A short (1-3 byte) MIDI message placed at a sample offset within a block. */
struct TimedMidi
{
    int sampleOffset = 0;
    std::uint8_t bytes[3]{};
    std::uint8_t size = 0;
    /** Where live input came from: computerKeyboardSource (the computer and on-screen keyboards)
        or a MIDI input's number (AudioEngine::midiSourceFor). Unused for other events. */
    std::uint8_t source = 0;
};

/** The source number of the computer and on-screen keyboards. */
inline constexpr std::uint8_t computerKeyboardSource = 0;

/** Live MIDI from any number of producer threads (MIDI input callbacks, the on-screen and computer
    keyboards, a control surface's input thread) to the audio thread.

    Lock-free on both sides (MpscQueue): a producer never waits for another one or for the audio
    thread, so a pad press on a controller's thread goes straight in (docs/06-m5-controllers.md,
    5b). Each message keeps when it arrived, so the engine plays it at the matching sample of the
    next block. Only short messages are carried (SysEx is ignored). */
class MidiInputQueue
{
public:
    static constexpr int capacity = 2048;

    /** Any thread. Returns false (and counts a drop) if the queue is full or the message is not a
        short message. arrivalSeconds: when it happened, on nowSeconds()'s clock (default: now). */
    bool push(const juce::MidiMessage& message, std::uint8_t source = computerKeyboardSource,
              double arrivalSeconds = -1.0) noexcept;
    /** The same from raw bytes (1 to 3), without a juce::MidiMessage. Real-time safe. */
    bool push(const std::uint8_t* bytes, int size, std::uint8_t source, double arrivalSeconds = -1.0) noexcept
        LATTICE_NONBLOCKING;

    /** Audio thread. Moves up to maxEvents queued messages into out (sampleOffset 0; the engine
        places them). Returns how many were written. If arrivalSeconds is given, it receives when
        each message arrived. */
    int popInto(TimedMidi* out, int maxEvents, double* arrivalSeconds = nullptr) noexcept LATTICE_NONBLOCKING;

    /** The clock arrival times are measured with (std::chrono::steady_clock, in seconds). */
    static double nowSeconds() noexcept LATTICE_NONBLOCKING;

    std::uint32_t droppedCount() const noexcept
    {
        return dropped.load(std::memory_order_relaxed) + fifo.droppedCount();
    }

private:
    struct Entry
    {
        TimedMidi event;
        double arrival = 0.0;
    };
    MpscQueue<Entry> fifo{capacity};
    std::atomic<std::uint32_t> dropped{0}; // not short messages
};

} // namespace lattice
