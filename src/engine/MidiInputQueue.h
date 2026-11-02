#pragma once

#include "core/RealtimeCheck.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

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
    keyboards) to the audio thread.

    Producers serialise among themselves with a mutex, which only non-real-time threads ever take;
    the audio thread is the single consumer of a lock-free FIFO, so it never locks or allocates.
    Replaces juce::MidiMessageCollector, which locks and can allocate on the audio thread.
    Only short messages are carried (SysEx is ignored). */
class MidiInputQueue
{
public:
    static constexpr int capacity = 2048;

    /** Any thread except the audio thread. Returns false (and counts a drop) if the queue is full
        or the message is not a short message. */
    bool push(const juce::MidiMessage& message, std::uint8_t source = computerKeyboardSource);

    /** Audio thread. Moves up to maxEvents queued messages into out, all at sampleOffset 0 (live
        input is played as soon as it arrives). Returns how many were written. If arrivalSeconds is
        given, it receives when each message was pushed (steady-clock seconds; see nowSeconds()),
        for the input latency probe. */
    int popInto(TimedMidi* out, int maxEvents, double* arrivalSeconds = nullptr) noexcept LATTICE_NONBLOCKING;

    /** The clock arrival times are measured with (std::chrono::steady_clock, in seconds). */
    static double nowSeconds() noexcept;

    std::uint32_t droppedCount() const noexcept { return dropped.load(std::memory_order_relaxed); }

private:
    std::mutex producerMutex;
    juce::AbstractFifo fifo{capacity};
    std::array<TimedMidi, capacity> buffer{};
    std::array<double, capacity> arrivals{};
    std::atomic<std::uint32_t> dropped{0};
};

} // namespace lattice
