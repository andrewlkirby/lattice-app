#pragma once

// Runs the same rendering once per SIMD level this machine has (docs/perf/avx2-plan.md,
// "Testing") and checks that the faster paths match the SSE2 one. Devices pick their kernels in
// prepare(), so `render` must create and prepare its device itself.

#include "devices/dsp/Simd.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace lattice::test
{

inline std::vector<dsp::SimdLevel> availableSimdLevels()
{
    std::vector<dsp::SimdLevel> levels;
    for (const auto level : {dsp::SimdLevel::Scalar, dsp::SimdLevel::Sse2, dsp::SimdLevel::Avx2})
        if (level <= dsp::cpuSimdLevel())
            levels.push_back(level);
    if (dsp::cpuSimdLevel() != dsp::SimdLevel::Scalar)
        levels.erase(levels.begin()); // x86-64: SSE2 is the floor
    return levels;
}

/** render() -> std::vector<float>, run at each level; every sample must match the lowest level's
    within `tolerance` of the signal's peak (float rounding differs between instruction sets). */
template <typename Render>
void checkSimdPathsMatch(Render&& render, float tolerance = 1.0e-5f)
{
    const auto levels = availableSimdLevels();
    std::vector<float> reference;
    {
        const dsp::ScopedSimdLevel scope(levels.front());
        reference = render();
    }
    float peak = 0.0f;
    for (const float v : reference)
        peak = std::max(peak, std::abs(v));
    REQUIRE(peak > 0.0f); // a silent render would prove nothing
    for (std::size_t l = 1; l < levels.size(); ++l)
    {
        INFO("SIMD level " << dsp::simdLevelName(levels[l]) << " against " << dsp::simdLevelName(levels.front()));
        const dsp::ScopedSimdLevel scope(levels[l]);
        const auto other = render();
        REQUIRE(other.size() == reference.size());
        float worst = 0.0f;
        for (std::size_t i = 0; i < reference.size(); ++i)
            worst = std::max(worst, std::abs(other[i] - reference[i]));
        CHECK(worst <= tolerance * peak);
    }
}

} // namespace lattice::test
