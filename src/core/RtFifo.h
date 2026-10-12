#pragma once

#include "core/RealtimeCheck.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <type_traits>
#include <vector>

namespace lattice
{

/** Fixed-capacity, lock-free, single-producer/single-consumer FIFO of trivially copyable items.
    Storage is allocated once, in the constructor; push and pop never allocate or block. A push to a
    full FIFO is dropped and counted. Either side may be the audio thread. */
template <typename T>
class SpscFifo
{
    static_assert(std::is_trivially_copyable_v<T>);

public:
    explicit SpscFifo(int capacity) : fifo(capacity), items(static_cast<std::size_t>(capacity)) {}

    SpscFifo(const SpscFifo&) = delete;
    SpscFifo& operator=(const SpscFifo&) = delete;

    /** Producer side. Returns false (and counts a drop) if the FIFO is full. */
    bool push(const T& item) noexcept LATTICE_NONBLOCKING
    {
        const auto scope = fifo.write(1);
        if (scope.blockSize1 + scope.blockSize2 < 1)
        {
            dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        items[static_cast<std::size_t>(scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2)] = item;
        return true;
    }

    /** Consumer side. Returns false if the FIFO is empty. */
    bool pop(T& item) noexcept LATTICE_NONBLOCKING
    {
        const auto scope = fifo.read(1);
        if (scope.blockSize1 + scope.blockSize2 < 1)
            return false;
        item = items[static_cast<std::size_t>(scope.blockSize1 > 0 ? scope.startIndex1 : scope.startIndex2)];
        return true;
    }

    /** Consumer side. Moves up to maxItems items into out; returns how many. */
    int popInto(T* out, int maxItems) noexcept LATTICE_NONBLOCKING
    {
        const auto scope = fifo.read(juce::jmin(maxItems, fifo.getNumReady()));
        int written = 0;
        for (int i = 0; i < scope.blockSize1; ++i)
            out[written++] = items[static_cast<std::size_t>(scope.startIndex1 + i)];
        for (int i = 0; i < scope.blockSize2; ++i)
            out[written++] = items[static_cast<std::size_t>(scope.startIndex2 + i)];
        return written;
    }

    int numReady() const noexcept { return fifo.getNumReady(); }
    /** The most items it can hold (one less than the storage size). */
    int capacity() const noexcept { return fifo.getTotalSize() - 1; }
    std::uint32_t droppedCount() const noexcept { return dropped.load(std::memory_order_relaxed); }

private:
    juce::AbstractFifo fifo;
    std::vector<T> items;
    std::atomic<std::uint32_t> dropped{0};
};

/** Many producers (never the audio thread), one lock-free consumer (typically the audio thread).
    Producers serialise among themselves with a mutex that the consumer never takes. */
template <typename T>
class MpscQueue
{
public:
    explicit MpscQueue(int capacity) : fifo(capacity) {}

    /** Any thread except the audio thread. Returns false (and counts a drop) if the queue is full. */
    bool push(const T& item)
    {
        RT_ASSERT_NOT_AUDIO_THREAD();
        const std::scoped_lock lock(producerMutex);
        return fifo.push(item);
    }

    int popInto(T* out, int maxItems) noexcept LATTICE_NONBLOCKING { return fifo.popInto(out, maxItems); }
    bool pop(T& item) noexcept LATTICE_NONBLOCKING { return fifo.pop(item); }
    std::uint32_t droppedCount() const noexcept { return fifo.droppedCount(); }

private:
    std::mutex producerMutex;
    SpscFifo<T> fifo;
};

} // namespace lattice
