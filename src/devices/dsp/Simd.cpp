#include "devices/dsp/Simd.h"

#include <juce_core/juce_core.h>

#include <algorithm>
#include <atomic>

namespace lattice::dsp
{

namespace
{
SimdLevel detect() noexcept
{
#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__)
    return juce::SystemStats::hasAVX2() && juce::SystemStats::hasFMA3() ? SimdLevel::Avx2 : SimdLevel::Sse2;
#else
    return SimdLevel::Scalar;
#endif
}

SimdLevel fromEnvironment(SimdLevel cpu) noexcept
{
    const auto requested = juce::SystemStats::getEnvironmentVariable("LATTICE_SIMD", {}).trim().toLowerCase();
    // SSE2 is the floor on x86-64 (it's always compiled in), so only AVX2 can be switched off.
    return requested == "sse2" ? std::min(SimdLevel::Sse2, cpu) : cpu;
}

std::atomic<SimdLevel>& current() noexcept
{
    static std::atomic<SimdLevel> level{fromEnvironment(cpuSimdLevel())};
    return level;
}
} // namespace

SimdLevel cpuSimdLevel() noexcept
{
    static const SimdLevel level = detect();
    return level;
}

SimdLevel simdLevel() noexcept
{
    return current().load(std::memory_order_relaxed);
}

void setSimdLevel(SimdLevel level) noexcept
{
    const auto cpu = cpuSimdLevel();
    current().store(std::clamp(level, cpu == SimdLevel::Scalar ? SimdLevel::Scalar : SimdLevel::Sse2, cpu),
                    std::memory_order_relaxed);
}

const char* simdLevelName(SimdLevel level) noexcept
{
    switch (level)
    {
    case SimdLevel::Scalar:
        return "scalar";
    case SimdLevel::Sse2:
        return "SSE2";
    case SimdLevel::Avx2:
        return "AVX2 + FMA";
    }
    return "?";
}

} // namespace lattice::dsp
