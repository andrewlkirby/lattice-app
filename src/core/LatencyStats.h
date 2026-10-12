#pragma once

#include "core/RealtimeCheck.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace lattice
{

/** Lock-free histogram of latencies in milliseconds (0.1 ms buckets up to 100 ms), written by one
    thread (the audio thread) and read by any. Used by the input latency probe
    (docs/09-performance-charter.md §2): the time from a MIDI message arriving to the block that
    plays it being handed to the driver. */
class LatencyStats
{
public:
    static constexpr double bucketMs = 0.1;
    static constexpr int numBuckets = 1001; // the last bucket holds everything >= 100 ms

    struct Summary
    {
        std::uint64_t count = 0;
        double p50Ms = 0.0;
        double p99Ms = 0.0;
        double maxMs = 0.0;
    };

    /** The single writer. */
    void record(double milliseconds) noexcept LATTICE_NONBLOCKING;

    /** Any thread except the writer's. Covers everything since the last reset(). */
    Summary summary() const noexcept;

    /** Message thread. Starts a new window; the writer is never blocked. */
    void reset() noexcept;

private:
    std::array<std::atomic<std::uint64_t>, numBuckets> buckets{};
    std::atomic<double> maxMs{0.0};
    std::atomic<bool> maxResetRequested{false};
    std::array<std::uint64_t, numBuckets> baseline{};
};

} // namespace lattice
