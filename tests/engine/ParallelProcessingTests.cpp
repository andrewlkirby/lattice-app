// Parallel track processing (docs/04-m3-devices.md, "Parallel track processing" and "Tests" 10):
// the reference set, with its sends, returns and sidechains, renders the same bits whatever the
// number of worker threads, and a block keeps its deadline with them.

#include "core/AudioCallbackStats.h"
#include "engine/WorkerPool.h"
#include "perf/ReferenceSet.h"
#include "platform/Platform.h"
#include "support/AllocationHooks.h"
#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

using namespace lattice;
using namespace lattice::test;

namespace
{
struct Rendered
{
    std::vector<float> left, right;
};

/** A fresh engine playing the reference set: scene 1, then scene 2 from `switchAt` (clips swap,
    voices release into the returns' tails). */
Rendered renderReferenceSet(int workers, BlockPattern pattern, std::int64_t samples, std::int64_t switchAt)
{
    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session(engine, tunings, testRegistry());
    OfflineRenderer renderer(engine, pattern);
    renderer.captureAudio = true;
    engine.setWorkerThreads(workers);
    perf::buildReferenceSet(session.set());
    session.flush();
    session.launchScene(0);
    session.flush();
    renderer.renderFor(switchAt);
    session.launchScene(1);
    session.flush();
    renderer.renderFor(samples - switchAt);
    return {renderer.audioLeft, renderer.audioRight};
}

/** The stress test's "audio thread": real-time priority (MMCSS Pro Audio on Windows), one block
    per period of the real clock, as a device callback would be. */
struct StressRender final : juce::Thread
{
    StressRender(AudioEngine& e, int blockSize, double periodSeconds, std::int64_t numBlocks)
        : juce::Thread("Stress audio"), engine(e), block(blockSize), period(periodSeconds), blocks(numBlocks)
    {
    }

    void run() override
    {
        platform::joinProAudioClass();
        std::vector<float> left(static_cast<std::size_t>(block)), right(static_cast<std::size_t>(block));
        float* outputs[2] = {left.data(), right.data()};
        for (int i = 0; i < 200; ++i) // warm up: caches, voices, the workers' first wake
            engine.process(outputs, 2, block);
        const auto start = std::chrono::steady_clock::now();
        const std::chrono::duration<double> periodDuration(period);
        for (std::int64_t i = 0; i < blocks && !threadShouldExit(); ++i)
        {
            const auto due = start + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                         periodDuration * static_cast<double>(i));
            while (std::chrono::steady_clock::now() < due)
                std::this_thread::yield();
            const auto t0 = std::chrono::steady_clock::now();
            engine.process(outputs, 2, block);
            const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            stats.recordCallback(took, period);
            worst = std::max(worst, took);
            if (took > period)
                ++late;
        }
        finished = true;
    }

    AudioEngine& engine;
    const int block;
    const double period;
    const std::int64_t blocks;
    AudioCallbackStats stats;
    double worst = 0.0;
    int late = 0;
    std::atomic<bool> finished{false};
};

double rms(const std::vector<float>& x)
{
    double s = 0.0;
    for (const float v : x)
        s += static_cast<double>(v) * v;
    return std::sqrt(s / static_cast<double>(std::max<std::size_t>(x.size(), 1)));
}

/** The first sample where two renders differ, or -1. */
std::int64_t firstDifference(const Rendered& a, const Rendered& b)
{
    const auto n = std::min(a.left.size(), b.left.size());
    for (std::size_t i = 0; i < n; ++i)
        if (std::memcmp(&a.left[i], &b.left[i], sizeof(float)) != 0
            || std::memcmp(&a.right[i], &b.right[i], sizeof(float)) != 0)
            return static_cast<std::int64_t>(i);
    return a.left.size() == b.left.size() ? -1 : static_cast<std::int64_t>(n);
}
} // namespace

TEST_CASE("WorkerPool: every task of every job runs exactly once, on any thread", "[engine][parallel]")
{
    for (const int workers : {0, 1, 3})
    {
        CAPTURE(workers);
        const WorkerPool pool(workers);
        struct Context
        {
            std::vector<std::atomic<int>> hits = std::vector<std::atomic<int>>(64);
        } context;
        for (int job = 0; job < 2000; ++job)
        {
            const int count = 1 + job % 40;
            for (auto& h : context.hits)
                h.store(0);
            pool.run(
                count,
                [](void* c, int i) noexcept {
                    auto& ctx = *static_cast<Context*>(c);
                    ctx.hits[static_cast<std::size_t>(i)].fetch_add(1);
                },
                &context);
            for (int i = 0; i < 64; ++i)
                REQUIRE(context.hits[static_cast<std::size_t>(i)].load() == (i < count ? 1 : 0));
        }
    }
}

TEST_CASE("Parallel processing: the reference set renders identical output with 0, 1, 2, 4 and N workers",
          "[engine][parallel][heavy]")
{
    // 1.5 s at 48 kHz in random block sizes, switching scenes after 0.75 s: every track plays,
    // four tracks duck under a sidechain, and every track sends to the returns.
    constexpr std::int64_t samples = 72000;
    constexpr std::int64_t switchAt = 36000;
    const auto reference = renderReferenceSet(0, BlockPattern::Random, samples, switchAt);
    REQUIRE(rms(reference.left) > 1.0e-3); // the set is really playing
    REQUIRE(rms(reference.right) > 1.0e-3);

    std::set<int> counts{1, 2, 4, std::max(1, WorkerPool::defaultWorkers())};
    for (const int workers : counts)
    {
        CAPTURE(workers);
        const auto parallel = renderReferenceSet(workers, BlockPattern::Random, samples, switchAt);
        CHECK(firstDifference(reference, parallel) == -1);
    }
}

TEST_CASE("Parallel processing: changing the worker count while playing doesn't change the output",
          "[engine][parallel][heavy]")
{
    constexpr std::int64_t samples = 48000;
    const auto reference = renderReferenceSet(0, BlockPattern::Fixed64, samples, samples / 2);

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session(engine, tunings, testRegistry());
    OfflineRenderer renderer(engine, BlockPattern::Fixed64);
    renderer.captureAudio = true;
    perf::buildReferenceSet(session.set());
    session.flush();
    session.launchScene(0);
    session.flush();
    const int steps[] = {2, 0, 4, 1, 3, 0};
    for (std::size_t i = 0; i < std::size(steps); ++i)
    {
        engine.setWorkerThreads(steps[i]);
        renderer.renderFor(samples / 2 / static_cast<std::int64_t>(std::size(steps)));
        engine.collectGarbage();
    }
    session.launchScene(1);
    session.flush();
    engine.setWorkerThreads(2);
    renderer.renderTo(samples);
    CHECK(firstDifference(reference, {renderer.audioLeft, renderer.audioRight}) == -1);
}

TEST_CASE("Parallel processing: the reference set plays 10 minutes at 64-sample blocks without a missed deadline",
          "[engine][parallel][stress][timing]")
{
    // docs/04-m3-devices.md, "Tests" 10: the offline harness with a real-time deadline simulated
    // per block. Tagged [timing]: it measures wall-clock time, so it runs in Release on the
    // reference machine (or a quiet one), not in CI. LATTICE_STRESS_SECONDS shortens it.
    constexpr double sampleRate = 48000.0;
    constexpr int block = 64;
    double seconds = 600.0;
    if (const auto env = juce::SystemStats::getEnvironmentVariable("LATTICE_STRESS_SECONDS", {}); env.isNotEmpty())
        seconds = std::max(1.0, env.getDoubleValue());

    TuningRegistry tunings;
    AudioEngine engine;
    engine.prepare(sampleRate, block);
    SessionController session(engine, tunings, testRegistry());
    const int workers = WorkerPool::defaultWorkers();
    engine.setWorkerThreads(workers);
    perf::buildReferenceSet(session.set());
    session.flush();
    session.launchScene(0);
    session.flush();

    // The "audio thread": real-time priority (MMCSS Pro Audio on Windows), one block per period of
    // the real clock, as a device callback would be (so the workers sleep between blocks and the
    // wake-up is measured too). Scene changes come from this (message) thread meanwhile.
    const double period = block / sampleRate;
    StressRender render(engine, block, period, static_cast<std::int64_t>(seconds * sampleRate / block));
    juce::Thread::RealtimeOptions options;
    if (!render.startRealtimeThread(options.withPriority(10)))
        render.startThread(juce::Thread::Priority::highest);
    int scene = 0;
    auto nextScene = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!render.finished)
    {
        juce::Thread::sleep(20);
        session.flush();
        engine.collectGarbage();
        if (std::chrono::steady_clock::now() >= nextScene)
        {
            session.launchScene(++scene % perf::referenceScenes);
            nextScene += std::chrono::seconds(30);
        }
    }
    render.stopThread(1000);
    const auto& stats = render.stats;
    const int late = render.late;
    const double worst = render.worst;
    const auto load = stats.summary();
    INFO(seconds << " s, " << workers << " workers: load p50 " << 100.0 * load.p50 << "%, p99 " << 100.0 * load.p99
                 << "%, p99.9 " << 100.0 * load.p999 << "%, worst block " << 1000.0 * worst << " ms of "
                 << 1000.0 * period << " ms; late blocks " << late);
    CHECK(late == 0);
    CHECK(load.p99 < 0.7); // the charter's headroom: 30% of the period left at the 99th percentile
    WARN("Load p50 " << 100.0 * load.p50 << "%, p99 " << 100.0 * load.p99 << "%, worst " << 100.0 * worst / period
                     << "%");
}
