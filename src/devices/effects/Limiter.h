#pragma once

#include "devices/framework/StudioDevice.h"

#include <vector>

namespace lattice
{

/** Audio effect "lattice.limiter" (docs/04-m3-devices.md, "Limiter"): a 1.5 ms look-ahead brickwall
    limiter, meant mainly for the master track. Its latency is reported, so delay compensation
    covers it.

    The gain is computed so that no sample can exceed the ceiling: for each incoming sample the
    gain it needs (ceiling / |peak|, both channels linked) goes into a sliding minimum over the
    look-ahead window; the minimum recovers towards 1 with the release time; and the result is
    averaged over the same window. Every value in that average already covers the loudest sample
    in reach, so the average is at most the gain that sample needs, while the averaging makes the
    gain move smoothly down to it over the look-ahead time (no instantaneous clipping). */
class LimiterDevice final : public StudioDevice
{
public:
    static constexpr const char* typeId = "lattice.limiter";
    static constexpr double lookaheadSeconds = 0.0015;

    /** Parameter indices, in declaration order. (In a struct, so the names don't fill the
        class scope.) */
    struct Param
    {
        enum : std::uint8_t
        {
            gain, ceiling, release, numParams
        };
    };

    LimiterDevice();

    /** The gain reduction applied most recently, in dB (<= 0), for meters. */
    float gainReductionDb() const noexcept { return reductionDb.load(std::memory_order_relaxed); }

protected:
    void prepare(double sampleRate, int maxBlockSize) override;
    void process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept override;
    void reset() noexcept override;

private:
    int window = 72; // look-ahead in samples
    // Look-ahead delay of the audio
    std::vector<float> delayL, delayR;
    int delayPos = 0;
    // Sliding minimum (monotonic deque in a ring buffer of window + 1 entries)
    std::vector<float> minValues;
    std::vector<std::int64_t> minTimes;
    int minHead = 0, minCount = 0;
    std::int64_t time = 0;
    // Release follower and moving average
    float envelope = 1.0f;
    std::vector<float> averageRing;
    int averagePos = 0;
    double averageSum = 0.0;
    float inputGain = 1.0f;
    std::atomic<float> reductionDb{0.0f};
};

} // namespace lattice
