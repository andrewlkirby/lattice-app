#include "core/AudioCallbackStats.h"

#include <algorithm>
#include <cmath>

namespace lattice
{

void AudioCallbackStats::recordCallback(double callbackSeconds, double periodSeconds) noexcept
{
    if (periodSeconds > 0.0)
        recordLoad(callbackSeconds / periodSeconds);
}

void AudioCallbackStats::recordLoad(double load) noexcept
{
    // Single writer: plain load + store is enough and avoids a locked read-modify-write.
    const auto index = std::clamp(static_cast<int>(load * bucketsPerPeriod), 0, numBuckets - 1);
    auto& bucket = buckets[static_cast<std::size_t>(index)];
    bucket.store(bucket.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);

    if (load > 1.0)
        overruns.store(overruns.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);

    if (maxResetRequested.load(std::memory_order_relaxed))
    {
        maxResetRequested.store(false, std::memory_order_relaxed);
        maxLoad.store(load, std::memory_order_relaxed);
    }
    else if (load > maxLoad.load(std::memory_order_relaxed))
    {
        maxLoad.store(load, std::memory_order_relaxed);
    }
}

AudioCallbackStats::Summary AudioCallbackStats::summary() const noexcept
{
    std::array<std::uint64_t, numBuckets> counts{};
    Summary s;
    for (std::size_t i = 0; i < counts.size(); ++i)
    {
        counts[i] = buckets[i].load(std::memory_order_relaxed) - baseline[i];
        s.callbacks += counts[i];
    }
    s.overruns = overruns.load(std::memory_order_relaxed) - overrunBaseline;
    s.max = maxResetRequested.load(std::memory_order_relaxed) ? 0.0 : maxLoad.load(std::memory_order_relaxed);

    if (s.callbacks == 0)
        return s;

    // Each percentile is reported as the upper edge of the bucket it falls in (conservative).
    const auto percentile = [&](double q) {
        const auto target = static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(s.callbacks)));
        std::uint64_t running = 0;
        for (std::size_t i = 0; i < counts.size(); ++i)
        {
            running += counts[i];
            if (running >= std::max<std::uint64_t>(target, 1))
                return static_cast<double>(i + 1) / bucketsPerPeriod;
        }
        return static_cast<double>(numBuckets) / bucketsPerPeriod;
    };
    s.p50 = percentile(0.50);
    s.p99 = percentile(0.99);
    s.p999 = percentile(0.999);
    // Max is exact while percentiles are bucket edges; a percentile can never exceed the max.
    if (s.max > 0.0)
    {
        s.p50 = std::min(s.p50, s.max);
        s.p99 = std::min(s.p99, s.max);
        s.p999 = std::min(s.p999, s.max);
    }
    return s;
}

void AudioCallbackStats::reset() noexcept
{
    for (std::size_t i = 0; i < baseline.size(); ++i)
        baseline[i] = buckets[i].load(std::memory_order_relaxed);
    overrunBaseline = overruns.load(std::memory_order_relaxed);
    maxResetRequested.store(true, std::memory_order_relaxed);
}

} // namespace lattice
