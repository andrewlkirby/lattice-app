#include "engine/MidiInputQueue.h"

#include <algorithm>
#include <chrono>

namespace lattice
{

bool MidiInputQueue::push(const juce::MidiMessage& message)
{
    RT_ASSERT_NOT_AUDIO_THREAD();

    const int size = message.getRawDataSize();
    if (size < 1 || size > 3 || message.isSysEx())
    {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    TimedMidi event;
    event.size = static_cast<std::uint8_t>(size);
    std::copy_n(message.getRawData(), size, event.bytes);

    const double arrival = nowSeconds();
    const std::scoped_lock lock(producerMutex);
    const auto scope = fifo.write(1);
    if (scope.blockSize1 + scope.blockSize2 < 1)
    {
        dropped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const auto index = static_cast<std::size_t>(scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2);
    buffer[index] = event;
    arrivals[index] = arrival;
    return true;
}

double MidiInputQueue::nowSeconds() noexcept
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

int MidiInputQueue::popInto(TimedMidi* out, int maxEvents, double* arrivalSeconds) noexcept
{
    const auto scope = fifo.read(juce::jmin(maxEvents, fifo.getNumReady()));
    int written = 0;
    const auto take = [&](int index) {
        const auto i = static_cast<std::size_t>(index);
        if (arrivalSeconds != nullptr)
            arrivalSeconds[written] = arrivals[i];
        out[written++] = buffer[i];
    };
    for (int i = 0; i < scope.blockSize1; ++i)
        take(scope.startIndex1 + i);
    for (int i = 0; i < scope.blockSize2; ++i)
        take(scope.startIndex2 + i);
    return written;
}

} // namespace lattice
