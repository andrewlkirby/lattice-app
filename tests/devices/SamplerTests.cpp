// The Sampler and the sample pool (docs/04-m3-devices.md, "Tests" 4).

#include "devices/dsp/SyncRates.h"
#include "devices/instruments/Sampler.h"
#include "support/AllocationHooks.h"
#include "support/DeviceHarness.h"
#include "support/Spectrum.h"
#include "tuning/Tet12.h"
#include "tuning/TuningRegistry.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace lattice;
using namespace lattice::test;

namespace
{
constexpr double sr = DeviceHarness::sampleRate;

std::vector<float> sine(double hz, double seconds, double rate = sr, float amplitude = 0.5f)
{
    std::vector<float> x(static_cast<std::size_t>(seconds * rate));
    for (std::size_t i = 0; i < x.size(); ++i)
        x[i] = amplitude * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * hz * static_cast<double>(i) / rate));
    return x;
}

SamplePtr monoSample(std::vector<float> x, double rate = sr)
{
    return SampleData::fromChannels({std::move(x)}, rate, "test");
}

/** Writes a mono 24-bit WAV and returns it (deleted with the object). */
struct TempWav
{
    explicit TempWav(const std::vector<float>& x, double rate = sr)
    {
        file.deleteFile();
        std::unique_ptr<juce::OutputStream> stream(file.createOutputStream().release());
        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor(
            stream, juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(1).withBitsPerSample(24));
        REQUIRE(writer != nullptr);
        const float* channels[1] = {x.data()};
        writer->writeFromFloatArrays(channels, 1, static_cast<int>(x.size()));
    }
    ~TempWav() { file.deleteFile(); }
    juce::File file = juce::File::createTempFile(".wav");
};

/** Waits for the pool's background loads and delivers their results (what the message loop does
    in the app), until `done` or a timeout. */
template <typename Done>
bool pumpUntil(SamplePool& pool, Done&& done, int timeoutMs = 5000)
{
    const auto until = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
    while (!done() && juce::Time::getMillisecondCounter() < until)
    {
        pool.waitUntilIdle();
        pool.deliverFinished();
    }
    return done();
}

struct Track
{
    explicit Track(TuningPtr t) : tuning(std::move(t)) { tuning.setSampleRate(sr); }
    TrackTuning tuning;
};
} // namespace

TEST_CASE("Sampler: a 440 Hz sample rooted at 440 Hz plays the Colundi table", "[devices][sampler][tuning]")
{
    Track track(TuningRegistry::makeBuiltinColundi());
    const auto colundi = TuningRegistry::makeBuiltinColundi();
    for (const int note : {12, 47, 60})
    {
        INFO("note " << note);
        SamplerDevice sampler(track.tuning, SamplePool::shared());
        setParam(sampler, "sample.root", 440.0f);
        DeviceHarness h(sampler);
        sampler.setSample(monoSample(sine(440.0, 10.0)));
        h.noteOn(0, note);
        h.renderTo(4800 + (1 << 16));
        const double expected = colundi->hz[static_cast<std::size_t>(note)];
        const auto [measured, level] = measureFundamental(h.slice(4800, 4800 + (1 << 16)), expected);
        CHECK(measured == Catch::Approx(expected).margin(0.05));
        CHECK(level > 0.9);
    }
    CHECK(colundi->hz[12] == Catch::Approx(59.9).margin(0.05));
}

TEST_CASE("Sampler: sinc interpolation pitched up two octaves is alias-free and flat", "[devices][sampler][dsp]")
{
    Track track(Tet12::make());
    const double binHz = sr / 65536.0;
    for (const bool draft : {false, true})
    {
        INFO((draft ? "Hermite (Draft)" : "sinc"));
        double worstAliasing = -400.0;
        double worstFlatness = 0.0;
        // Output tones from 1 kHz to 0.9 x Nyquist, on FFT bins. The sample is at 44.1 kHz, so +24
        // steps plays it at 4 x 44.1 / 48 = 3.675 input samples per output sample: fractional
        // positions everywhere, as in real use.
        constexpr double fileRate = 44100.0;
        const double ratio = 4.0 * fileRate / sr;
        for (const int outBin : {1366, 4096, 13653, 21845, 26624})
        {
            const double outHz = outBin * binHz;
            SamplerDevice sampler(track.tuning, SamplePool::shared());
            setParam(sampler, "sample.tuning", 1.0f); // fixed 12-TET: note 84 is exactly 4x
            setParam(sampler, "sample.quality", draft ? 1.0f : 0.0f);
            setParam(sampler, "amp.velocity", 0.0f);
            setParam(sampler, "out.volume", 0.0f);
            DeviceHarness h(sampler);
            sampler.setSample(monoSample(sine(outHz / 4.0, 8.0, fileRate, 0.5f), fileRate));
            juce::ignoreUnused(ratio);
            h.noteOn(0, 84, 127);
            h.renderTo(4800 + (1 << 16));
            const auto signal = h.slice(4800, 4800 + (1 << 16));
            worstAliasing = std::max(worstAliasing, aliasingDb(signal, outHz, sr, 16, true));
            const auto spectrum = amplitudeSpectrum(signal, 16, true);
            // The device's output stage: unity at centre pan.
            const double level = 20.0 * std::log10(spectrum[static_cast<std::size_t>(outBin)] / 0.5);
            worstFlatness = std::max(worstFlatness, std::abs(level));
        }
        INFO("worst aliasing " << worstAliasing << " dB, worst level error " << worstFlatness << " dB");
        if (!draft)
        {
            CHECK(worstAliasing <= -90.0);
            CHECK(worstFlatness <= 0.1);
        }
        else
        {
            CHECK(worstAliasing > -90.0); // the reason sinc is the default
        }
    }
}

TEST_CASE("Sampler: slice mode puts each slice on its note", "[devices][sampler]")
{
    Track track(Tet12::make());
    // Four bursts, half a second apart: a sharp attack each.
    std::vector<float> x(static_cast<std::size_t>(2.0 * sr), 0.0f);
    const std::int64_t starts[4] = {0, 24000, 48000, 72000};
    for (int b = 0; b < 4; ++b)
        for (int i = 0; i < 12000; ++i)
            x[static_cast<std::size_t>(starts[b] + i)] =
                (0.8f - 0.1f * static_cast<float>(b)) * std::exp(-i / 3000.0f) *
                static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * 200.0 * (b + 1) * i / sr));

    SamplerDevice sampler(track.tuning, SamplePool::shared());
    setParam(sampler, "sample.mode", static_cast<float>(SamplerDevice::Mode::Slice));
    sampler.setSample(monoSample(x));

    SECTION("by transient")
    {
        const auto points = sampler.slicePoints();
        REQUIRE(points.size() == 4);
        for (int b = 0; b < 4; ++b)
            CHECK(std::abs(points[static_cast<std::size_t>(b)] - starts[b]) <= 512);

        // Note 38 plays the third burst, at its pitch (600 Hz), and nothing of the others.
        DeviceHarness h(sampler);
        h.noteOn(0, SamplerDevice::firstSliceNote + 2);
        h.renderTo(24000);
        CHECK(zeroCrossingHz(h.audioLeft, 1000, 6000) == Catch::Approx(600.0).epsilon(0.01));
        CHECK(peakOf(h.audioLeft, 23000, 24000) < 1e-3f); // the slice ended before the fourth burst
    }

    SECTION("by beat")
    {
        setParam(sampler, "slice.by", static_cast<float>(SamplerDevice::SliceBy::Beat));
        setParam(sampler, "slice.tempo", 120.0f);
        setParam(sampler, "slice.division", static_cast<float>(dsp::SyncRates::indexOf("1/4")));
        const auto points = sampler.slicePoints();
        REQUIRE(points.size() == 4); // 2 s at 120 BPM in quarter notes
        for (int b = 0; b < 4; ++b)
            CHECK(points[static_cast<std::size_t>(b)] == b * 24000);
    }

    SECTION("by hand")
    {
        setParam(sampler, "slice.by", static_cast<float>(SamplerDevice::SliceBy::Manual));
        juce::ValueTree state("STATE");
        state.setProperty("slices", "30000,60000", nullptr);
        sampler.setState(state);
        const auto points = sampler.slicePoints();
        REQUIRE(points.size() == 3);
        CHECK(points[1] == 30000);
        CHECK(points[2] == 60000);
    }
}

TEST_CASE("Sampler: one-shot ignores note-off; classic releases", "[devices][sampler]")
{
    Track track(Tet12::make());
    for (const bool oneShot : {true, false})
    {
        INFO((oneShot ? "one-shot" : "classic"));
        SamplerDevice sampler(track.tuning, SamplePool::shared());
        setParam(sampler, "sample.mode", static_cast<float>(oneShot ? SamplerDevice::Mode::OneShot : SamplerDevice::Mode::Classic));
        setParam(sampler, "amp.release", 10.0f);
        setParam(sampler, "sample.tuning", 1.0f);
        DeviceHarness h(sampler);
        sampler.setSample(monoSample(sine(300.0, 1.0)));
        h.noteOn(0, 60);
        h.noteOff(2400, 60);
        h.renderTo(24000);
        const float late = peakOf(h.audioLeft, 20000, 24000);
        if (oneShot)
            CHECK(late > 0.1f);
        else
            CHECK(late == 0.0f);
    }
}

TEST_CASE("Sampler: while a file loads it is silent and allocates nothing on the audio thread",
          "[devices][sampler][rt]")
{
    LATTICE_REQUIRE_ALLOCATION_HOOKS();
    Track track(Tet12::make());
    const TempWav wav(sine(440.0, 3.0));
    SamplePool pool;
    SamplerDevice sampler(track.tuning, pool);
    setParam(sampler, "sample.tuning", 1.0f);
    DeviceHarness h(sampler);

    juce::ValueTree state("STATE");
    state.setProperty("file", wav.file.getFullPathName(), nullptr);
    sampler.setState(state);
    CHECK(sampler.isLoading());

    const auto before = rt::violationCount();
    h.noteOn(0, 60);
    h.renderTo(4800); // the load runs on a background thread meanwhile
    CHECK(peakOf(h.audioLeft) == 0.0f);

    REQUIRE(pumpUntil(pool, [&] { return !sampler.isLoading(); }));
    CHECK(sampler.lastError().isEmpty());
    REQUIRE(sampler.currentSample() != nullptr);
    CHECK(sampler.currentSample()->length == static_cast<std::int64_t>(3.0 * sr));
    h.noteOn(h.now(), 60);
    h.renderTo(h.now() + 24000);
    CHECK(peakOf(h.audioLeft, 4800) > 0.1f);
    CHECK(rt::violationCount() == before);
}

TEST_CASE("Sample pool: one file loaded into two devices is one copy in memory", "[devices][sampler]")
{
    Track track(Tet12::make());
    const TempWav wav(sine(220.0, 1.0));
    SamplePool pool;
    auto a = pool.loadNow(wav.file);
    auto b = pool.loadNow(wav.file);
    REQUIRE(a != nullptr);
    CHECK(a == b);
    CHECK(pool.numLoaded() == 1);

    SamplerDevice first(track.tuning, pool);
    SamplerDevice second(track.tuning, pool);
    juce::ValueTree state("STATE");
    state.setProperty("file", wav.file.getFullPathName(), nullptr);
    first.setState(state);
    second.setState(state);
    REQUIRE(pumpUntil(pool, [&] { return !first.isLoading() && !second.isLoading(); }));
    CHECK(first.currentSample() == a);
    CHECK(second.currentSample() == a);
    CHECK(pool.numLoaded() == 1);

    a.reset();
    b.reset();
    first.setSample(nullptr);
    second.setSample(nullptr);
    // Unused now (published versions are freed once collected).
    juce::ValueTree empty("STATE");
    first.setState(empty);
    second.setState(empty);
}

TEST_CASE("Sampler: loop with crossfade plays on smoothly", "[devices][sampler]")
{
    Track track(Tet12::make());
    SamplerDevice sampler(track.tuning, SamplePool::shared());
    setParam(sampler, "sample.tuning", 1.0f);
    setParam(sampler, "loop.on", 1.0f);
    setParam(sampler, "loop.start", 0.25f);
    setParam(sampler, "loop.length", 0.5f);
    setParam(sampler, "loop.crossfade", 50.0f);
    DeviceHarness h(sampler);
    // A sine whose loop points don't line up with its cycles: without the crossfade the loop clicks.
    sampler.setSample(monoSample(sine(437.0, 0.5)));
    h.noteOn(0, 60);
    h.renderTo(static_cast<std::int64_t>(sr * 3.0));
    const float level = peakOf(h.audioLeft, 4800);
    REQUIRE(level > 0.1f);
    CHECK(peakOf(h.audioLeft, h.audioLeft.size() - 4800) > 0.5f * level); // still playing after 6 loops
    CHECK(largestStep(h.audioLeft, 1000) < 0.1f * level);
}
