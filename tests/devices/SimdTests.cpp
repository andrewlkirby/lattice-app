// The SIMD level and its kernels (docs/perf/avx2-plan.md): devices pick their kernels from
// dsp::simdLevel() when they prepare, and every faster path matches the SSE2 one.

#include "devices/dsp/Simd.h"
#include "devices/dsp/SincBlock.h"
#include "devices/instruments/Sampler.h"
#include "support/DeviceHarness.h"
#include "support/SimdPaths.h"
#include "tuning/Tet12.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace lattice;
using namespace lattice::test;

TEST_CASE("SIMD level: follows the CPU, can be lowered for comparison, never raised", "[devices][dsp][simd]")
{
    const auto cpu = dsp::cpuSimdLevel();
    const auto start = dsp::simdLevel();
    CHECK(start <= cpu);
    {
        const dsp::ScopedSimdLevel scope(dsp::SimdLevel::Avx2);
        CHECK(dsp::simdLevel() == cpu); // never above what the CPU has
    }
    {
        const dsp::ScopedSimdLevel scope(dsp::SimdLevel::Scalar);
        // SSE2 is the floor on x86-64 (it is compiled in); only ARM builds run scalar.
        CHECK(dsp::simdLevel() == (cpu == dsp::SimdLevel::Scalar ? dsp::SimdLevel::Scalar : dsp::SimdLevel::Sse2));
    }
    CHECK(dsp::simdLevel() == start); // restored
    CHECK(juce::String(dsp::simdLevelName(cpu)).isNotEmpty());

    // The sinc kernel follows the level.
    CHECK(dsp::sincBlockFor(dsp::SimdLevel::Sse2) == &dsp::sincBlockGeneric);
    if (cpu == dsp::SimdLevel::Avx2)
        CHECK(dsp::sincBlockFor(dsp::SimdLevel::Avx2) != &dsp::sincBlockGeneric);
}

TEST_CASE("SIMD paths: the Sampler sounds the same at every level", "[devices][sampler][simd]")
{
    // Noise, so every tap of the kernel matters; voices pitched down, near unity and up (several
    // kernel levels), at awkward block sizes.
    std::vector<float> noise(48000);
    std::uint32_t seed = 7;
    for (auto& x : noise)
    {
        seed = seed * 1664525u + 1013904223u;
        x = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
    }
    const auto sample = SampleData::fromChannels({noise, noise}, 44100.0, "noise");
    static TrackTuning tuning(Tet12::make());
    tuning.setSampleRate(DeviceHarness::sampleRate);

    for (const float quality : {0.0f, 1.0f}) // sinc, Draft
    {
        CAPTURE(quality);
        checkSimdPathsMatch([&] {
            SamplerDevice sampler(tuning, SamplePool::shared());
            sampler.findParameter("sample.quality")->setBase(quality);
            sampler.setSample(sample);
            DeviceHarness h(sampler);
            for (const int note : {40, 59, 60, 61, 72, 84, 90})
                h.noteOn(0, note);
            h.renderTo(12000, 37);
            std::vector<float> out = h.audioLeft;
            out.insert(out.end(), h.audioRight.begin(), h.audioRight.end());
            return out;
        });
    }
}
