// The SIMD level and its kernels (docs/perf/avx2-plan.md): devices pick their kernels from
// dsp::simdLevel() when they prepare, and every faster path matches the SSE2 one.

#include "devices/dsp/OscillatorKernels.h"
#include "devices/dsp/Simd.h"
#include "devices/dsp/SincBlock.h"
#include "devices/dsp/VoiceFilterSimd.h"
#include "devices/dsp/Wavetable.h"
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

namespace
{
/** A job for one copy of `wave` at `inc`, as the Synth builds them. */
dsp::OscCopyJob oscJob(const dsp::MipmappedWave& wave, int level, float blend, std::uint32_t phase, std::uint32_t inc,
                       std::int32_t step, std::uint32_t pulseOffset, float gl, float gr)
{
    const int next = std::min(level + 1, dsp::MipmappedWave::numLevels - 1);
    return {wave.levelData(level) + dsp::MipmappedWave::guard - 1,
            wave.levelData(next) + dsp::MipmappedWave::guard - 1,
            wave.levelBits(level),
            wave.levelBits(next),
            next > level ? blend : 0.0f,
            pulseOffset,
            gl,
            gr,
            step,
            phase,
            inc};
}

/** The reference: MipmappedWave's own reads, one sample at a time. */
void renderReference(const dsp::MipmappedWave& wave, int level, dsp::OscCopyJob& j, int n, float* outL, float* outR)
{
    const auto value = [&](std::uint32_t ph) {
        const float a = wave.readLevel(level, ph);
        return j.blend > 0.0f ? a + j.blend * (wave.readLevel(level + 1, ph) - a) : a;
    };
    std::uint32_t p = j.phase;
    auto v = static_cast<std::int64_t>(j.inc);
    for (int i = 0; i < n; ++i)
    {
        v += j.step;
        float x = value(p);
        if (j.pulseOffset != 0)
            x -= value(p + j.pulseOffset);
        p += static_cast<std::uint32_t>(v);
        outL[i] += x * j.gainL;
        outR[i] += x * j.gainR;
    }
    j.phase = p;
    j.inc = static_cast<std::uint32_t>(v);
}
} // namespace

TEST_CASE("SIMD paths: the oscillator kernels match MipmappedWave's reads at any group count and block size",
          "[devices][dsp][synth][simd]")
{
    const auto& library = dsp::WaveformLibrary::get();
    const dsp::MipmappedWave* waves[] = {&library.shape(dsp::WaveformLibrary::Shape::Saw),
                                         &library.shape(dsp::WaveformLibrary::Shape::Sine),
                                         &library.shape(dsp::WaveformLibrary::Shape::Triangle)};
    std::mt19937 rng(5);
    std::uniform_int_distribution<std::uint32_t> anyPhase;
    std::uniform_real_distribution<float> unit(0.0f, 1.0f);

    for (const auto level : availableSimdLevels())
    {
        INFO("SIMD level " << dsp::simdLevelName(level));
        const auto kernels = dsp::oscillatorKernelsFor(level);
        for (const int groups : {1, 2, 3, 4})
            for (const int n : {1, 3, 8, 13, 37, 64})
            {
                INFO(groups << " groups, " << n << " samples");
                // Each group: its own shape, level (so table size), pulse or not; each copy its own
                // pitch near the group's, blend, phase and gains.
                std::vector<dsp::OscCopyJob> jobs, reference;
                std::vector<int> levels;
                for (int g = 0; g < groups; ++g)
                {
                    const auto& wave = *waves[g % 3];
                    const int tableLevel = 4 + 5 * g;
                    const std::uint32_t pulse = g % 2 == 1 ? anyPhase(rng) | 1u : 0u;
                    for (int k = 0; k < 4; ++k)
                    {
                        const auto inc = static_cast<std::uint32_t>(std::ldexp(1.0, -14 + tableLevel / 2) * (1.0 + 0.1 * k));
                        jobs.push_back(oscJob(wave, tableLevel, k == 0 ? 0.0f : unit(rng), anyPhase(rng), inc,
                                              static_cast<std::int32_t>(inc / 3000) * (k % 2 == 0 ? 1 : -1), pulse,
                                              unit(rng), k % 3 == 0 ? 0.0f : unit(rng)));
                        levels.push_back(tableLevel);
                    }
                }
                reference = jobs;
                std::vector<float> l(64, 0.1f), r(64, -0.2f), lRef = l, rRef = r;
                for (int pass = 0; pass < 3; ++pass) // phases carried across blocks
                {
                    kernels.groups(jobs.data(), static_cast<int>(jobs.size()), n, l.data(), r.data());
                    for (std::size_t k = 0; k < reference.size(); ++k)
                        renderReference(*waves[(k / 4) % 3], levels[k], reference[k], n, lRef.data(), rRef.data());
                }
                float worst = 0.0f;
                for (std::size_t i = 0; i < l.size(); ++i)
                    worst = std::max({worst, std::abs(l[i] - lRef[i]), std::abs(r[i] - rRef[i])});
                CHECK(worst < 2.0e-5f);
                for (std::size_t k = 0; k < jobs.size(); ++k)
                {
                    CHECK(jobs[k].phase == reference[k].phase);
                    CHECK(jobs[k].inc == reference[k].inc);
                }

                // The same copies one at a time.
                auto singles = reference;
                auto singlesRef = reference;
                std::vector<float> sl(64, 0.0f), sr(64, 0.0f), slRef = sl, srRef = sr;
                for (std::size_t k = 0; k < singles.size(); ++k)
                {
                    kernels.copy(singles[k], n, sl.data(), sr.data());
                    renderReference(*waves[(k / 4) % 3], levels[k], singlesRef[k], n, slRef.data(), srRef.data());
                    CHECK(singles[k].phase == singlesRef[k].phase);
                    CHECK(singles[k].inc == singlesRef[k].inc);
                }
                worst = 0.0f;
                for (std::size_t i = 0; i < sl.size(); ++i)
                    worst = std::max({worst, std::abs(sl[i] - slRef[i]), std::abs(sr[i] - srRef[i])});
                CHECK(worst < 2.0e-5f);
            }
    }
}

TEST_CASE("SIMD paths: the Synth's oscillators sound the same at every level", "[devices][synth][simd]")
{
    static TrackTuning tuning(Tet12::make());
    tuning.setSampleRate(DeviceHarness::sampleRate);
    struct Case
    {
        int unison;
        float shape1, shape2; // SynthDevice::Shape
        float spread;
    };
    for (const auto c : {Case{1, 2.0f, 3.0f, 0.0f}, Case{3, 3.0f, 0.0f, 1.0f}, Case{4, 2.0f, 3.0f, 1.0f},
                         Case{4, 1.0f, 2.0f, 0.0f}, Case{8, 2.0f, 2.0f, 0.7f}, Case{6, 3.0f, 1.0f, 1.0f}})
    {
        CAPTURE(c.unison, c.shape1, c.shape2, c.spread);
        checkSimdPathsMatch([&] {
            SynthDevice synth(tuning);
            synth.findParameter("unison.voices")->setBase(static_cast<float>(c.unison));
            synth.findParameter("unison.spread")->setBase(c.spread);
            synth.findParameter("osc1.shape")->setBase(c.shape1);
            synth.findParameter("osc2.shape")->setBase(c.shape2);
            synth.findParameter("osc2.pw")->setBase(0.3f);
            DeviceHarness h(synth);
            for (int n = 0; n < 6; ++n) // low to very high, so the copies sit on many table levels
                h.noteOn(0, 30 + 15 * n);
            h.renderTo(9600, 37);
            std::vector<float> out = h.audioLeft;
            out.insert(out.end(), h.audioRight.begin(), h.audioRight.end());
            return out;
        });
    }
}

TEST_CASE("SIMD paths: oscillator kernel timings", "[.][bench][simd]")
{
    // ns per copy per sample: lattice_tests "[bench][simd]". 16 voices x 8 copies of a saw (two
    // oscillators x unison 4, a group each), as the Synth's unison-4 benchmark plays them: either all
    // at 220 Hz (one small table, cached) or on the benchmark's notes (MIDI 36-81, so the low ones
    // read the large tables).
    const auto& saw = dsp::WaveformLibrary::get().shape(dsp::WaveformLibrary::Shape::Saw);
    constexpr int block = 64;
    constexpr int voices = 16;
    for (const auto level : availableSimdLevels())
        for (const bool spread : {false, true})
            for (const int mode : {0, 1}) // groups, one copy at a time
            {
                const auto kernels = dsp::oscillatorKernelsFor(level);
                std::vector<dsp::OscCopyJob> jobs;
                for (int voice = 0; voice < voices; ++voice)
                    for (int c = 0; c < 8; ++c)
                    {
                        const double hz = spread ? 440.0 * std::exp2((36 + 3 * voice - 69) / 12.0) : 220.0;
                        const double ratio = (c < 4 ? 1.0 : 1.004) * (1.0 + 0.003 * (c % 4));
                        const auto inc = static_cast<std::uint32_t>(4294967296.0 * hz * ratio / 48000.0);
                        // Each group on its first copy's level, as the Synth does.
                        const auto at = dsp::MipmappedWave::positionFor(
                            static_cast<std::uint32_t>(4294967296.0 * hz * (c < 4 ? 1.0 : 1.004) / 48000.0));
                        jobs.push_back(oscJob(saw, at.level, at.blend, static_cast<std::uint32_t>(c) * 0x1234567u, inc, 0,
                                              0, 0.3f, 0.2f));
                    }
                std::vector<float> l(block), r(block);
                constexpr int blocks = 12500;
                const auto t0 = std::chrono::steady_clock::now();
                for (int b = 0; b < blocks; ++b)
                    for (int voice = 0; voice < voices; ++voice)
                    {
                        auto* group = jobs.data() + voice * 8;
                        if (mode == 0)
                            kernels.groups(group, 8, block, l.data(), r.data());
                        else
                            for (int c = 0; c < 8; ++c)
                                kernels.copy(group[c], block, l.data(), r.data());
                    }
                const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                std::printf("%-14s %-6s %-14s %.2f ns per copy-sample (%.3f)\n", dsp::simdLevelName(level),
                            mode == 0 ? "groups" : "copy", spread ? "notes 36-81" : "all at 220 Hz",
                            1.0e9 * seconds / (static_cast<double>(blocks) * voices * 8 * block), l[5]);
            }
}

TEST_CASE("SIMD paths: sinc kernel timings", "[.][bench][simd]")
{
    // ns per output sample: lattice_tests "[bench][simd]". 32 voices, 64 positions per block,
    // either on the Sampler benchmark's notes (MIDI 48-79 from a root at 60, a 44.1 kHz sample at
    // 48 kHz: 12 kernel levels, whose tables don't fit in L2) or all on the unity level (one table).
    const auto& bank = dsp::SincKernelBank::get();
    const int pad = bank.maxTaps() / 2 + 8;
    const std::size_t length = 1 << 16;
    std::vector<float> left(length + 2 * static_cast<std::size_t>(pad)), right(left.size());
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> noise(-1.0f, 1.0f);
    for (std::size_t i = 0; i < length; ++i)
    {
        left[i + static_cast<std::size_t>(pad)] = noise(rng);
        right[i + static_cast<std::size_t>(pad)] = noise(rng);
    }
    const float* s0 = left.data() + pad;
    const float* s1 = right.data() + pad;
    constexpr int voices = 32;
    constexpr int block = 64;
    for (const bool spread : {true, false})
        for (const auto level : availableSimdLevels())
            for (const bool stereo : {true, false})
            {
                double rates[voices];
                double positions[voices];
                double taps = 0.0;
                for (int v = 0; v < voices; ++v)
                {
                    rates[v] = spread ? std::exp2((48 + v - 60) / 12.0) * 44100.0 / 48000.0 : 0.9 + 0.001 * v;
                    positions[v] = 1000.0 + 37.3 * v;
                    taps += bank.levelFor(rates[v]).taps;
                }
                const auto kernel = dsp::sincBlockFor(level);
                std::int64_t index[block];
                float fraction[block];
                float outL[block], outR[block];
                float check = 0.0f;
                constexpr int blocks = 4000;
                double busy = 0.0;
                for (int b = 0; b < blocks; ++b)
                    for (int v = 0; v < voices; ++v)
                    {
                        double x = positions[v];
                        for (int i = 0; i < block; ++i)
                        {
                            index[i] = static_cast<std::int64_t>(x);
                            fraction[i] = static_cast<float>(x - std::floor(x));
                            x += rates[v];
                        }
                        positions[v] = x >= static_cast<double>(length - 2000) ? 1000.0 : x;
                        const auto view = bank.levelFor(rates[v]).view();
                        const auto t0 = std::chrono::steady_clock::now();
                        kernel(view, s0, stereo ? s1 : s0, index, fraction, block, outL, outR);
                        busy += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
                        check += outL[3] + outR[7];
                    }
                const double samples = static_cast<double>(blocks) * voices * block;
                std::printf("%-14s %-6s %-14s %.2f ns per sample, %.3f ns per tap (%.3f)\n", dsp::simdLevelName(level),
                            stereo ? "stereo" : "mono", spread ? "notes 48-79" : "unity level", 1.0e9 * busy / samples,
                            1.0e9 * busy / samples / (taps / voices), check);
            }
}
