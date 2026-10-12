#pragma once

#include "core/RealtimeCheck.h"

#include <array>
#include <atomic>
#include <cstdint>

namespace lattice
{

/** Lock-free histogram of audio-callback duration as a fraction of the buffer period
    (docs/09-performance-charter.md §2). One writer (the audio thread) and any number of readers.

    The charter's budget is p99.9 <= 70% of the period. A callback that takes longer than the whole
    period is counted as an overrun: the device is very likely to have dropped audio. */
class AudioCallbackStats
{
public:
    /** Bucket width is 0.5% of the buffer period; loads >= 200% share the last bucket. */
    static constexpr int bucketsPerPeriod = 200;
    static constexpr int numBuckets = 2 * bucketsPerPeriod + 1;

    struct Summary
    {
        std::uint64_t callbacks = 0;
        std::uint64_t overruns = 0; // callbacks that took longer than one buffer period
        double p50 = 0.0;           // fractions of the buffer period (1.0 = 100%)
        double p99 = 0.0;
        double p999 = 0.0;
        double max = 0.0;
    };

    /** Audio thread. callbackSeconds is how long the callback took; periodSeconds is
        numSamples / sampleRate. */
    void recordCallback(double callbackSeconds, double periodSeconds) noexcept LATTICE_NONBLOCKING;

    /** Audio thread. load is the callback's duration as a fraction of the buffer period. */
    void recordLoad(double load) noexcept LATTICE_NONBLOCKING;

    /** Any thread except the audio thread. Values cover everything recorded since the last reset(). */
    Summary summary() const noexcept;

    /** Message thread. Starts a new measurement window. The writer is never blocked. */
    void reset() noexcept;

private:
    std::array<std::atomic<std::uint64_t>, numBuckets> buckets{};
    std::atomic<std::uint64_t> overruns{0};
    std::atomic<double> maxLoad{0.0};
    std::atomic<bool> maxResetRequested{false};

    // Reader-side baseline taken at reset(); subtracted from the running totals.
    std::array<std::uint64_t, numBuckets> baseline{};
    std::uint64_t overrunBaseline = 0;
};

} // namespace lattice
