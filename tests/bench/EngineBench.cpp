// Engine benchmark (docs/09-performance-charter.md §2 and §7, M2): renders the reference set
// offline and reports how much faster than real time it ran and the per-block load. Under
// callgrind (scripts/linux-test.sh --callgrind) only renderReferenceSet() is counted, which gives
// an instruction count that doesn't depend on how fast or busy the machine is.
//
//   lattice_engine_bench [--seconds N] [--block N] [--write-set file.lset]

#include "core/AudioCallbackStats.h"
#include "engine/AudioEngine.h"
#include "engine/SessionController.h"
#include "perf/ReferenceSet.h"

#include <juce_events/juce_events.h>

#include <chrono>
#include <cstdio>
#include <vector>

namespace
{
constexpr double sampleRate = 48000.0;

struct Result
{
    double renderSeconds = 0.0;
    lattice::AudioCallbackStats::Summary load;
};

#if defined(__GNUC__) || defined(__clang__)
#define LATTICE_BENCH_NOINLINE __attribute__((noinline))
#else
#define LATTICE_BENCH_NOINLINE __declspec(noinline)
#endif

/** Everything callgrind counts. */
LATTICE_BENCH_NOINLINE Result renderReferenceSet(lattice::AudioEngine& engine, int blockSize,
                                                 std::int64_t totalSamples)
{
    std::vector<float> left(static_cast<std::size_t>(blockSize)), right(static_cast<std::size_t>(blockSize));
    float* outputs[2] = {left.data(), right.data()};
    lattice::AudioCallbackStats stats;
    const double period = blockSize / sampleRate;

    const auto start = std::chrono::steady_clock::now();
    for (std::int64_t done = 0; done < totalSamples; done += blockSize)
    {
        const auto t0 = std::chrono::steady_clock::now();
        engine.process(outputs, 2, blockSize);
        const auto t1 = std::chrono::steady_clock::now();
        stats.recordCallback(std::chrono::duration<double>(t1 - t0).count(), period);
    }
    Result r;
    r.renderSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    r.load = stats.summary();
    return r;
}
} // namespace

int main(int argc, char* argv[])
{
    const juce::ScopedJuceInitialiser_GUI juceInit; // the session controller uses timers

    double seconds = 10.0;
    int blockSize = 64;
    juce::String writeSet;
    for (int i = 1; i + 1 < argc; i += 2)
    {
        const juce::String arg(argv[i]);
        const juce::String value(argv[i + 1]);
        if (arg == "--seconds")
            seconds = value.getDoubleValue();
        else if (arg == "--block")
            blockSize = std::max(1, value.getIntValue());
        else if (arg == "--write-set")
            writeSet = value;
    }

    lattice::TuningRegistry tunings;
    lattice::AudioEngine engine;
    engine.prepare(sampleRate, blockSize);
    lattice::SessionController session(engine, tunings);
    lattice::perf::buildReferenceSet(session.set());
    session.markClean();

    if (writeSet.isNotEmpty())
    {
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(writeSet);
        const auto result = session.saveSet(file);
        std::printf("%s %s\n", result.wasOk() ? "Wrote" : "Could not write",
                    file.getFullPathName().toRawUTF8());
        return result.wasOk() ? 0 : 1;
    }

    session.launchScene(0);
    const auto totalSamples = static_cast<std::int64_t>(seconds * sampleRate);
    const auto r = renderReferenceSet(engine, blockSize, totalSamples);

    std::printf("Reference set (%d tracks, M2 test-synth placeholders), %d-sample blocks at %.0f Hz\n",
                lattice::perf::referenceTracks, blockSize, sampleRate);
    std::printf("Rendered %.1f s of audio in %.3f s: %.1fx real time\n", seconds, r.renderSeconds,
                seconds / std::max(r.renderSeconds, 1.0e-9));
    std::printf(
        "Per-block load (share of the %.2f ms period): p50 %.1f%%  p99 %.1f%%  p99.9 %.1f%%  max %.1f%%\n",
        1000.0 * blockSize / sampleRate, 100.0 * r.load.p50, 100.0 * r.load.p99, 100.0 * r.load.p999,
        100.0 * r.load.max);
    return 0;
}
