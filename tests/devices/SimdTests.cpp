// The SIMD level and its kernels (docs/perf/avx2-plan.md): devices pick their kernels from
// dsp::simdLevel() when they prepare, and every faster path matches the SSE2 one.

#include "devices/dsp/Simd.h"
#include "devices/dsp/SincBlock.h"
#include "devices/dsp/VoiceFilterSimd.h"
#include "devices/instruments/Sampler.h"
#include "devices/instruments/Synth.h"
#include "support/DeviceHarness.h"
#include "support/SimdPaths.h"
#include "tuning/Tet12.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <random>

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

    // So do the voice filter kernels.
    CHECK(dsp::voiceFilterKernelsFor(dsp::SimdLevel::Sse2).ladder == dsp::voiceFilterKernelsGeneric().ladder);
    if (cpu == dsp::SimdLevel::Avx2)
        CHECK(dsp::voiceFilterKernelsFor(dsp::SimdLevel::Avx2).ladder != dsp::voiceFilterKernelsGeneric().ladder);
}

TEST_CASE("SIMD paths: the voice filter kernels match the scalar filters at any lane count and block size",
          "[devices][dsp][filter][simd]")
{
    constexpr int maxLanes = 33;
    constexpr int maxBlock = 64;
    struct Kind
    {
        bool ladder;
        bool steep;
        dsp::SvfMode mode;
    };
    const Kind kinds[] = {{true, false, dsp::SvfMode::LowPass},   {false, false, dsp::SvfMode::LowPass},
                          {false, true, dsp::SvfMode::LowPass},   {false, false, dsp::SvfMode::HighPass},
                          {false, true, dsp::SvfMode::HighPass},  {false, false, dsp::SvfMode::BandPass},
                          {false, false, dsp::SvfMode::Notch}};
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);

    for (const auto level : availableSimdLevels())
    {
        INFO("SIMD level " << dsp::simdLevelName(level));
        dsp::VoiceFilterSimd filters;
        filters.prepare(level);
        for (const auto& kind : kinds)
            for (const int lanes : {1, 2, 3, 4, 5, 7, 8, 9, 12, 15, 16, 17, 24, 31, 32, 33})
                for (const int block : {1, 3, 8, 13, 37, 64})
                {
                    INFO((kind.ladder ? "ladder" : kind.steep ? "SVF 24 dB" : "SVF 12 dB")
                         << " mode " << static_cast<int>(kind.mode) << ", " << lanes << " lanes, " << block
                         << "-sample blocks");
                    std::array<dsp::Svf, maxLanes> svfA{}, svfB{};
                    std::array<dsp::LadderFilter, maxLanes> ladderA{}, ladderB{};
                    std::array<std::array<float, maxBlock>, maxLanes> bufA{}, bufB{};
                    double worst = 0.0;
                    for (int b = 0; b < 6; ++b) // state carried across blocks
                    {
                        std::array<dsp::FilterLane, maxLanes> lanesA{}, lanesB{};
                        for (int l = 0; l < lanes; ++l)
                        {
                            const auto ul = static_cast<std::size_t>(l);
                            for (int i = 0; i < block; ++i)
                                bufA[ul][static_cast<std::size_t>(i)] = bufB[ul][static_cast<std::size_t>(i)] =
                                    1.5f * noise(rng);
                            const double fc = 60.0 * std::pow(2.0, (l * 5 + b) % 17 * 0.55);
                            const float g = kind.ladder ? dsp::LadderFilter::prewarpOversampled(fc, DeviceHarness::sampleRate)
                                                        : dsp::SvfCoefficients::prewarp(fc, DeviceHarness::sampleRate);
                            for (auto* set : {&lanesA, &lanesB})
                            {
                                auto& lane = (*set)[ul];
                                lane.g = g;
                                lane.gStep = g * (l % 2 == 0 ? 0.002f : -0.001f);
                                lane.resonance = static_cast<float>(l % 6) * 0.17f;
                            }
                            lanesA[ul].buffer = bufA[ul].data();
                            lanesB[ul].buffer = bufB[ul].data();
                            lanesA[ul].svf = &svfA[ul];
                            lanesB[ul].svf = &svfB[ul];
                            lanesA[ul].ladder = &ladderA[ul];
                            lanesB[ul].ladder = &ladderB[ul];
                        }
                        if (kind.ladder)
                        {
                            filters.ladder(lanesA.data(), lanes, block, 2.5f);
                            for (int l = 0; l < lanes; ++l)
                                dsp::VoiceFilterSimd::ladderScalar(lanesB[static_cast<std::size_t>(l)], block, 2.5f);
                        }
                        else
                        {
                            filters.svf(lanesA.data(), lanes, block, kind.steep, kind.mode, 1.5f);
                            for (int l = 0; l < lanes; ++l)
                                dsp::VoiceFilterSimd::svfScalar(lanesB[static_cast<std::size_t>(l)], block, kind.steep,
                                                                kind.mode, 1.5f);
                        }
                        for (int l = 0; l < lanes; ++l)
                            for (int i = 0; i < block; ++i)
                                worst = std::max(worst, static_cast<double>(std::abs(
                                                            bufA[static_cast<std::size_t>(l)][static_cast<std::size_t>(i)] -
                                                            bufB[static_cast<std::size_t>(l)][static_cast<std::size_t>(i)])));
                    }
                    CHECK(worst < 1.0e-4);
                }
    }
}

TEST_CASE("SIMD paths: the Synth's filters sound the same at every level", "[devices][synth][simd]")
{
    static TrackTuning tuning(Tet12::make());
    tuning.setSampleRate(DeviceHarness::sampleRate);
    for (const auto type : {SynthDevice::FilterType::Ladder, SynthDevice::FilterType::LowPass24,
                            SynthDevice::FilterType::BandPass})
    {
        CAPTURE(static_cast<int>(type));
        checkSimdPathsMatch([&] {
            SynthDevice synth(tuning);
            synth.findParameter("filter.type")->setBase(static_cast<float>(type));
            synth.findParameter("filter.resonance")->setBase(0.6f);
            synth.findParameter("filter.drive")->setBase(6.0f); // dB
            synth.findParameter("filter.envAmount")->setBase(0.5f);
            synth.findParameter("unison.voices")->setBase(2.0f); // stereo voices: two lanes each
            DeviceHarness h(synth);
            for (int n = 0; n < 11; ++n) // 22 lanes: a pair of registers and a padded one
                h.noteOn(0, 36 + 3 * n);
            h.renderTo(9600, 37);
            std::vector<float> out = h.audioLeft;
            out.insert(out.end(), h.audioRight.begin(), h.audioRight.end());
            return out;
        });
    }
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

TEST_CASE("SIMD paths: voice filter kernel timings", "[.][bench][simd]")
{
    // ns per lane per sample, for each level and lane count: lattice_tests "[bench][simd]". 4 and 8
    // lanes fill one register (or half an AVX2 one); 16 and 32 run two registers interleaved.
    constexpr int maxLanes = 32;
    constexpr int block = 64;
    for (const int lanes : {4, 8, 16, 32})
        for (const auto level : availableSimdLevels())
            for (const int kind : {0, 1, 2}) // ladder, SVF 12 dB, SVF 24 dB
            {
                dsp::VoiceFilterSimd filters;
                filters.prepare(level);
                std::array<dsp::Svf, maxLanes> svf{};
                std::array<dsp::LadderFilter, maxLanes> ladder{};
                std::array<std::array<float, block>, maxLanes> buf{};
                std::array<dsp::FilterLane, maxLanes> lane{};
                for (int l = 0; l < lanes; ++l)
                {
                    const auto ul = static_cast<std::size_t>(l);
                    lane[ul] = {buf[ul].data(), &svf[ul], &ladder[ul],
                                dsp::LadderFilter::prewarpOversampled(200.0 + 150.0 * l, 48000.0), 0.0f, 0.5f};
                }
                constexpr int blocks = 40000;
                double seconds = 0.0;
                for (int b = 0; b < blocks; ++b)
                {
                    for (int l = 0; l < lanes; ++l)
                        for (int i = 0; i < block; ++i)
                            buf[static_cast<std::size_t>(l)][static_cast<std::size_t>(i)] =
                                0.1f * static_cast<float>((i * 7919 + b * 31) % 200 - 100) / 100.0f;
                    const auto t0 = std::chrono::steady_clock::now();
                    if (kind == 0)
                        filters.ladder(lane.data(), lanes, block, 1.0f);
                    else
                        filters.svf(lane.data(), lanes, block, kind == 2, dsp::SvfMode::LowPass, 1.0f);
                    seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                }
                std::printf("%2d lanes  %-14s %-10s %.2f ns per lane-sample\n", lanes, dsp::simdLevelName(level),
                            kind == 0 ? "ladder" : kind == 1 ? "SVF 12 dB" : "SVF 24 dB",
                            1.0e9 * seconds / (static_cast<double>(blocks) * block * lanes));
            }
}
