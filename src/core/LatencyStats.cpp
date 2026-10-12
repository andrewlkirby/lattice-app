#include "core/LatencyStats.h"

#include <algorithm>
#include <cmath>

namespace lattice
{

void LatencyStats::record(double milliseconds) noexcept
{
    const auto index = std::clamp(static_cast<int>(milliseconds / bucketMs), 0, numBuckets - 1);
    auto& bucket = buckets[static_cast<std::size_t>(index)];
    bucket.store(bucket.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);

    if (maxResetRequested.load(std::memory_order_relaxed))
    {
        maxResetRequested.store(false, std::memory_order_relaxed);
        maxMs.store(milliseconds, std::memory_order_relaxed);
    }
    else if (milliseconds > maxMs.load(std::memory_order_relaxed))
    {
        maxMs.store(milliseconds, std::memory_order_relaxed);
    }
}

LatencyStats::Summary LatencyStats::summary() const noexcept
{
    std::array<std::uint64_t, numBuckets> counts{};
    Summary s;
    for (std::size_t i = 0; i < counts.size(); ++i)
    {
        counts[i] = buckets[i].load(std::memory_order_relaxed) - baseline[i];
        s.count += counts[i];
    }
    if (s.count == 0)
        return s;
    s.maxMs = maxResetRequested.load(std::memory_order_relaxed) ? 0.0 : maxMs.load(std::memory_order_relaxed);

    // Percentiles are reported as the upper edge of their bucket (conservative).
    const auto percentile = [&](double q) {
        const auto target = std::max<std::uint64_t>(
            1, static_cast<std::uint64_t>(std::ceil(q * static_cast<double>(s.count))));
        std::uint64_t running = 0;
        for (std::size_t i = 0; i < counts.size(); ++i)
        {
            running += counts[i];
            if (running >= target)
                return static_cast<double>(i + 1) * bucketMs;
        }
        return static_cast<double>(numBuckets) * bucketMs;
    };
    s.p50Ms = percentile(0.5);
    s.p99Ms = percentile(0.99);
    return s;
}

void LatencyStats::reset() noexcept
{
    for (std::size_t i = 0; i < baseline.size(); ++i)
        baseline[i] = buckets[i].load(std::memory_order_relaxed);
    maxResetRequested.store(true, std::memory_order_relaxed);
}

} // namespace lattice
