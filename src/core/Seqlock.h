#pragma once

#include "core/RealtimeCheck.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <type_traits>

namespace lattice
{

/** Publishes a small trivially copyable value from one writer (e.g. the audio thread) to any
    number of readers, which always see a consistent copy. The writer never waits; a reader retries
    if a write overlapped its read. The value is kept in atomic words, so this is free of data races
    as far as the C++ memory model (and ThreadSanitizer) is concerned. */
template <typename T>
class Seqlock
{
    static_assert(std::is_trivially_copyable_v<T>);
    static constexpr std::size_t numWords = (sizeof(T) + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t);

public:
    Seqlock() { store(T{}); }

    /** The single writer. Wait-free. */
    void store(const T& value) noexcept LATTICE_NONBLOCKING
    {
        std::array<std::uint64_t, numWords> buffer{};
        std::memcpy(buffer.data(), static_cast<const void*>(&value), sizeof(T));

        const auto s = sequence.load(std::memory_order_relaxed);
        sequence.store(s + 1, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_release);
        for (std::size_t i = 0; i < numWords; ++i)
            words[i].store(buffer[i], std::memory_order_relaxed);
        sequence.store(s + 2, std::memory_order_release);
    }

    /** Any reader (not the writer's thread while it is writing). */
    T load() const noexcept
    {
        std::array<std::uint64_t, numWords> buffer{};
        for (int attempt = 0;; ++attempt)
        {
            const auto before = sequence.load(std::memory_order_acquire);
            if ((before & 1u) == 0)
            {
                for (std::size_t i = 0; i < numWords; ++i)
                    buffer[i] = words[i].load(std::memory_order_relaxed);
                std::atomic_thread_fence(std::memory_order_acquire);
                if (sequence.load(std::memory_order_relaxed) == before)
                    break;
            }
            if (attempt > 64)
                std::this_thread::yield();
        }
        T value;
        std::memcpy(static_cast<void*>(&value), buffer.data(), sizeof(T));
        return value;
    }

private:
    std::atomic<std::uint32_t> sequence{0};
    std::array<std::atomic<std::uint64_t>, numWords> words{};
};

} // namespace lattice
