#include "engine/MidiInputQueue.h"

#include <algorithm>
#include <chrono>

namespace lattice
{

bool MidiInputQueue::push(const juce::MidiMessage& message, std::uint8_t source, double arrivalSeconds) noexcept
{
    if (message.isSysEx())
    {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return push(message.getRawData(), message.getRawDataSize(), source, arrivalSeconds);
}

bool MidiInputQueue::push(const std::uint8_t* bytes, int size, std::uint8_t source, double arrivalSeconds,
                          std::uint32_t target, std::uint8_t flags) noexcept
{
    if (size < 1 || size > 3 || bytes[0] == 0xf0)
    {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    Entry entry;
    entry.event.size = static_cast<std::uint8_t>(size);
    entry.event.source = source;
    entry.event.target = target;
    entry.event.flags = flags;
    std::copy_n(bytes, size, entry.event.bytes);
    entry.arrival = arrivalSeconds >= 0.0 ? arrivalSeconds : nowSeconds();
    return fifo.push(entry);
}

double MidiInputQueue::nowSeconds() noexcept
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int MidiInputQueue::popInto(TimedMidi* out, int maxEvents, double* arrivalSeconds) noexcept
{
    int written = 0;
    Entry entry;
    while (written < maxEvents && fifo.pop(entry))
    {
        if (arrivalSeconds != nullptr)
            arrivalSeconds[written] = entry.arrival;
        out[written++] = entry.event;
    }
    return written;
}

} // namespace lattice
