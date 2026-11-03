#pragma once

#include "core/RealtimeCheck.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <cstddef>
#include <memory>
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

    /** Producer side. Pushes as many of the items as fit; returns how many (the rest count as
        dropped). */
    int pushMany(const T* source, int count) noexcept LATTICE_NONBLOCKING
    {
        const auto scope = fifo.write(juce::jmin(count, fifo.getFreeSpace()));
        int read = 0;
        for (int i = 0; i < scope.blockSize1; ++i)
            items[static_cast<std::size_t>(scope.startIndex1 + i)] = source[read++];
        for (int i = 0; i < scope.blockSize2; ++i)
            items[static_cast<std::size_t>(scope.startIndex2 + i)] = source[read++];
        if (read < count)
            dropped.fetch_add(static_cast<std::uint32_t>(count - read), std::memory_order_relaxed);
        return read;
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

/** Many producers on any threads, one consumer (typically the audio thread), with no lock on
    either side: a bounded queue in which each cell carries a sequence number (Vyukov's). A producer
    claims a cell with one compare-exchange and never waits for another producer, so a MIDI input
    thread, the message thread and a controller's thread can all push at once
    (docs/06-m5-controllers.md, 5b). Storage is allocated once, in the constructor; push and pop
    never allocate or block. A push to a full queue is dropped and counted.

    A producer that is preempted between claiming a cell and filling it holds back what was pushed
    after it until it finishes: the consumer stops at that cell and takes the rest next time. */
template <typename T>
class MpscQueue
{
    static_assert(std::is_trivially_copyable_v<T>);

public:
    /** Holds at least `capacity` items (rounded up to a power of two). */
    explicit MpscQueue(int capacity)
        : size(roundUp(capacity)), mask(size - 1), cells(std::make_unique<Cell[]>(size))
    {
        for (std::size_t i = 0; i < size; ++i)
            cells[i].sequence.store(i, std::memory_order_relaxed);
    }

    MpscQueue(const MpscQueue&) = delete;
    MpscQueue& operator=(const MpscQueue&) = delete;

    /** Any thread. Returns false (and counts a drop) if the queue is full. */
    bool push(const T& item) noexcept LATTICE_NONBLOCKING
    {
        auto pos = tail.load(std::memory_order_relaxed);
        for (;;)
        {
            auto& cell = cells[pos & mask];
            const auto sequence = cell.sequence.load(std::memory_order_acquire);
            const auto diff = static_cast<std::ptrdiff_t>(sequence - pos);
            if (diff == 0)
            {
                if (tail.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
                {
                    cell.item = item;
                    cell.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            }
            else if (diff < 0)
            {
                dropped.fetch_add(1, std::memory_order_relaxed);
                return false; // full: the consumer hasn't freed this cell yet
            }
            else
            {
                pos = tail.load(std::memory_order_relaxed);
            }
        }
    }

    /** The consumer. Returns false if nothing is ready. */
    bool pop(T& item) noexcept LATTICE_NONBLOCKING
    {
        auto& cell = cells[head & mask];
        if (cell.sequence.load(std::memory_order_acquire) != head + 1)
            return false; // empty, or a producer is still filling it
        item = cell.item;
        cell.sequence.store(head + size, std::memory_order_release);
        ++head;
        return true;
    }

    /** The consumer. Moves up to maxItems ready items into out, oldest first; returns how many. */
    int popInto(T* out, int maxItems) noexcept LATTICE_NONBLOCKING
    {
        int written = 0;
        while (written < maxItems && pop(out[written]))
            ++written;
        return written;
    }

    /** The most items it can hold. */
    int capacity() const noexcept { return static_cast<int>(size); }
    std::uint32_t droppedCount() const noexcept { return dropped.load(std::memory_order_relaxed); }

private:
    struct Cell
    {
        std::atomic<std::size_t> sequence{0};
        T item{};
    };
    static std::size_t roundUp(int capacity)
    {
        std::size_t n = 2;
        while (n < static_cast<std::size_t>(juce::jmax(2, capacity)))
            n <<= 1;
        return n;
    }

    const std::size_t size;
    const std::size_t mask;
    std::unique_ptr<Cell[]> cells;
    alignas(64) std::atomic<std::size_t> tail{0}; // producers
    alignas(64) std::size_t head = 0;             // the consumer only
    std::atomic<std::uint32_t> dropped{0};
};

} // namespace lattice
