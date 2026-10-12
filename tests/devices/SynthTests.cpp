// The Synth (docs/04-m3-devices.md, "Tests" 3).

#include "devices/instruments/Synth.h"
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
using P = SynthDevice::Param;

/** A synth with one plain sine oscillator through a wide-open filter. */
void makePlainSine(SynthDevice& synth)
{
    setParam(synth, "osc1.shape", static_cast<float>(SynthDevice::Shape::Sine));
    setParam(synth, "osc1.level", 0.0f);
    setParam(synth, "osc2.level", -70.0f);
    setParam(synth, "filter.cutoff", 20000.0f);
    setParam(synth, "filter.keytrack", 0.0f);
    setParam(synth, "filter.resonance", 0.0f);
    setParam(synth, "filter.type", static_cast<float>(SynthDevice::FilterType::LowPass12));
    setParam(synth, "amp.sustain", 1.0f);
}

struct ColundiTrack
{
    ColundiTrack()
    {
        tuning.setTuning(TuningRegistry::makeBuiltinColundi());
        tuning.setSampleRate(DeviceHarness::sampleRate);
    }
    TrackTuning tuning;
};

std::vector<float> welchSpectrum(const std::vector<float>& signal, int order)
{
    const int size = 1 << order;
    std::vector<double> sum;
    int segments = 0;
    for (std::size_t start = 0; start + static_cast<std::size_t>(size) <= signal.size(); start += static_cast<std::size_t>(size / 2))
    {
        const std::vector<float> segment(signal.begin() + static_cast<std::ptrdiff_t>(start),
                                         signal.begin() + static_cast<std::ptrdiff_t>(start) + size);
        const auto a = amplitudeSpectrum(segment, order);
        if (sum.empty())
            sum.assign(a.size(), 0.0);
        for (std::size_t i = 0; i < a.size(); ++i)
            sum[i] += a[i] * a[i];
        ++segments;
    }
    std::vector<float> power(sum.size());
    for (std::size_t i = 0; i < sum.size(); ++i)
        power[i] = static_cast<float>(sum[i] / segments);
    return power;
}
} // namespace

TEST_CASE("Synth: Colundi notes sound at the table's frequencies", "[devices][synth][tuning]")
{
    ColundiTrack track;
    const auto colundi = TuningRegistry::makeBuiltinColundi();
    for (const int note : {12, 47, 100})
    {
        INFO("note " << note);
        SynthDevice synth(track.tuning);
        makePlainSine(synth);
        DeviceHarness h(synth);
        h.noteOn(0, note);
        h.renderTo(4800 + (1 << 16));
        const auto [measured, level] = measureFundamental(h.slice(4800, 4800 + (1 << 16)), colundi->hz[static_cast<std::size_t>(note)]);
        CHECK(measured == Catch::Approx(colundi->hz[static_cast<std::size_t>(note)]).margin(0.5));
        CHECK(level > 0.9);
    }
}

TEST_CASE("Synth: glide moves along the tuning table", "[devices][synth][tuning]")
{
    ColundiTrack track;
    const auto colundi = TuningRegistry::makeBuiltinColundi();
    SynthDevice synth(track.tuning);
    makePlainSine(synth);
    setParam(synth, "voice.mode", static_cast<float>(SynthDevice::VoiceMode::Legato));
    setParam(synth, "voice.glide", 1000.0f); // 1 s from note 40 to note 50
    DeviceHarness h(synth);
    h.noteOn(0, 40);
    h.noteOn(48000, 50);
    h.renderTo(48000 * 3);

    // Halfway through, the pitch is note 45: the table's own entry, not a straight line in Hz.
    const double atHalf = zeroCrossingHz(h.audioLeft, 48000 + 24000 - 1200, 48000 + 24000 + 1200);
    CHECK(atHalf == Catch::Approx(colundi->hz[45]).epsilon(0.01));
    // A quarter of the way: note 42.5, between the entries in log terms.
    const double atQuarter = zeroCrossingHz(h.audioLeft, 48000 + 12000 - 1200, 48000 + 12000 + 1200);
    CHECK(atQuarter == Catch::Approx(colundi->frequencyFor(42.5)).epsilon(0.01));
    // Before and after: the notes themselves.
    CHECK(zeroCrossingHz(h.audioLeft, 24000, 47000) == Catch::Approx(colundi->hz[40]).epsilon(0.002));
    CHECK(zeroCrossingHz(h.audioLeft, 48000 * 2 + 4800, 48000 * 3) == Catch::Approx(colundi->hz[50]).epsilon(0.002));
}

TEST_CASE("Synth: filter key tracking at 100% doubles the cutoff when the note's frequency doubles",
          "[devices][synth][filter]")
{
    TrackTuning twelveTet(Tet12::make());
    const auto cutoffFor = [&](int note) {
        SynthDevice synth(twelveTet);
        setParam(synth, "osc1.level", -70.0f);
        setParam(synth, "osc2.level", -70.0f);
        setParam(synth, "noise.level", 0.0f);
        setParam(synth, "noise.colour", 1.0f);
        setParam(synth, "filter.type", static_cast<float>(SynthDevice::FilterType::LowPass12));
        setParam(synth, "filter.cutoff", 1000.0f);
        setParam(synth, "filter.resonance", 0.0f);
        setParam(synth, "filter.keytrack", 1.0f);
        setParam(synth, "amp.sustain", 1.0f);
        DeviceHarness h(synth);
        h.noteOn(0, note);
        h.renderTo(48000 * 8);
        const auto power = welchSpectrum(h.slice(4800, 48000 * 8), 12);
        const double binHz = 48000.0 / 4096.0;
        // The pass band's level (well below the cutoff) and where it has fallen by 3 dB.
        double passband = 0.0;
        int count = 0;
        for (int b = 3; b < static_cast<int>(100.0 / binHz) + 3; ++b, ++count)
            passband += power[static_cast<std::size_t>(b)];
        passband /= count;
        for (std::size_t b = 10; b < power.size(); ++b)
        {
            // Smooth over a few bins against the noise's variance.
            double local = 0.0;
            for (std::size_t k = b - 2; k <= b + 2; ++k)
                local += power[k];
            local /= 5.0;
            if (local < 0.5 * passband)
                return b * binHz;
        }
        return 0.0;
    };
    const double at60 = cutoffFor(60); // 261.6 Hz
    const double at72 = cutoffFor(72); // 523.3 Hz: double
    INFO("cutoffs " << at60 << " and " << at72);
    CHECK(at72 / at60 == Catch::Approx(2.0).epsilon(0.05));
    // At middle C the cutoff is the setting itself.
    CHECK(at60 == Catch::Approx(1000.0).epsilon(0.05));
}

TEST_CASE("Synth: voice stealing doesn't click", "[devices][synth]")
{
    TrackTuning twelveTet(Tet12::make());
    SynthDevice synth(twelveTet);
    makePlainSine(synth);
    setParam(synth, "voice.polyphony", 2.0f);
    setParam(synth, "amp.release", 2000.0f);
    DeviceHarness h(synth);
    // Many more notes than voices, each held: every new one steals.
    for (int i = 0; i < 40; ++i)
        h.noteOn(1000 + i * 1201, 57 + (i * 5) % 12, 100);
    h.renderTo(1000 + 40 * 1201 + 4800, 0);

    const float level = peakOf(h.audioLeft);
    REQUIRE(level > 0.05f);
    // Two sines near 220-415 Hz move by at most level * 2 pi * 415 / 48000 = 0.054 * level per
    // sample. A stolen voice cut off at once would jump by up to its full amplitude.
    CHECK(largestStep(h.audioLeft) < 0.15f * level);
    CHECK(synth.activeVoiceCount() <= 2 + SynthDevice::spareVoices);
}

TEST_CASE("Synth: the ladder at full resonance stays bounded", "[devices][synth][filter]")
{
    TrackTuning twelveTet(Tet12::make());
    SynthDevice synth(twelveTet);
    setParam(synth, "filter.type", static_cast<float>(SynthDevice::FilterType::Ladder));
    setParam(synth, "filter.resonance", 1.0f);
    setParam(synth, "filter.drive", 24.0f);
    setParam(synth, "osc1.level", 0.0f);
    setParam(synth, "osc2.level", 0.0f);
    DeviceHarness h(synth);
    for (int i = 0; i < 8; ++i)
        h.noteOn(i * 100, 36 + i * 7, 127);
    h.renderTo(48000 * 2);
    for (const float v : h.audioLeft)
        REQUIRE(std::isfinite(v));
    CHECK(peakOf(h.audioLeft) < 4.0f);
}

TEST_CASE("Synth: mono, legato, sustain pedal", "[devices][synth]")
{
    TrackTuning twelveTet(Tet12::make());

    // Attack and decay 100 ms to a sustain of 50 %: by 0.5 s the voice is at half level.
    const auto shortEnvelope = [](SynthDevice& synth) {
        makePlainSine(synth);
        setParam(synth, "amp.attack", 100.0f);
        setParam(synth, "amp.decay", 100.0f);
        setParam(synth, "amp.sustain", 0.5f);
    };

    SECTION("legato: a second key while the first is held changes pitch without retriggering")
    {
        SynthDevice synth(twelveTet);
        shortEnvelope(synth);
        setParam(synth, "voice.mode", static_cast<float>(SynthDevice::VoiceMode::Legato));
        DeviceHarness h(synth);
        h.noteOn(0, 60);
        h.noteOn(24000, 64);
        h.renderTo(48000);
        CHECK(synth.activeVoiceCount() == 1);
        const float held = peakOf(h.audioLeft, 22000, 24000);
        CHECK(peakOf(h.audioLeft, 28000, 30000) == Catch::Approx(held).epsilon(0.05)); // still sustaining
        CHECK(zeroCrossingHz(h.audioLeft, 30000, 48000) == Catch::Approx(329.63).epsilon(0.002));
        // Releasing the newer key goes back to the one still held.
        h.noteOff(48000, 64);
        h.renderTo(48000 * 2);
        CHECK(zeroCrossingHz(h.audioLeft, 48000 + 4800, 48000 * 2) == Catch::Approx(261.63).epsilon(0.002));
    }

    SECTION("mono: a second key retriggers the envelope (from where it is, so without a click)")
    {
        SynthDevice synth(twelveTet);
        shortEnvelope(synth);
        setParam(synth, "voice.mode", static_cast<float>(SynthDevice::VoiceMode::Mono));
        DeviceHarness h(synth);
        h.noteOn(0, 60);
        h.noteOn(24000, 64);
        h.renderTo(48000);
        const float held = peakOf(h.audioLeft, 22000, 24000);
        CHECK(peakOf(h.audioLeft, 28000, 29600) > 1.8f * held); // the attack rose to full level again
        CHECK(largestStep(h.audioLeft, 23000, 25000) < 0.05f);
    }

    SECTION("the sustain pedal holds released notes until it is lifted")
    {
        SynthDevice synth(twelveTet);
        makePlainSine(synth);
        setParam(synth, "amp.release", 10.0f);
        DeviceHarness h(synth);
        h.controller(0, 64, 127);
        h.noteOn(10, 60);
        h.noteOff(4800, 60);
        h.renderTo(24000);
        CHECK(peakOf(h.audioLeft, 20000, 24000) > 0.1f); // still sounding
        h.controller(24000, 64, 0);
        h.renderTo(48000);
        CHECK(peakOf(h.audioLeft, 30000, 48000) == 0.0f);
        CHECK(synth.activeVoiceCount() == 0);
    }
}

TEST_CASE("Synth: unison spreads copies across the stereo field", "[devices][synth]")
{
    TrackTuning twelveTet(Tet12::make());
    SynthDevice synth(twelveTet);
    setParam(synth, "unison.voices", 4.0f);
    setParam(synth, "unison.spread", 1.0f);
    DeviceHarness h(synth);
    h.noteOn(0, 48);
    h.renderTo(48000);
    double difference = 0.0;
    double total = 0.0;
    for (std::size_t i = 4800; i < h.audioLeft.size(); ++i)
    {
        difference += std::abs(h.audioLeft[i] - h.audioRight[i]);
        total += std::abs(h.audioLeft[i]) + std::abs(h.audioRight[i]);
    }
    CHECK(difference > 0.2 * total); // left and right differ: it is wide
}

TEST_CASE("Synth: random parameter changes while playing stay finite and never allocate",
          "[devices][synth][rt]")
{
    LATTICE_REQUIRE_ALLOCATION_HOOKS();
    ColundiTrack track;
    SynthDevice synth(track.tuning);
    DeviceHarness h(synth);
    juce::Random random(42);
    const auto before = rt::violationCount();
    for (int step = 0; step < 300; ++step)
    {
        const std::int64_t at = h.now();
        for (int k = 0; k < 4; ++k)
        {
            auto& p = synth.parameter(random.nextInt(synth.numParameters()));
            const auto& s = p.spec();
            p.setBase(s.minValue + random.nextFloat() * (s.maxValue - s.minValue));
        }
        if (random.nextInt(3) == 0)
            h.noteOn(at + random.nextInt(500), random.nextInt(128), 1 + random.nextInt(127));
        if (random.nextInt(3) == 0)
            h.noteOff(at + random.nextInt(500), random.nextInt(128));
        h.renderTo(at + 1 + random.nextInt(2000), 0);
    }
    CHECK(rt::violationCount() == before);
    for (const float v : h.audioLeft)
        REQUIRE(std::isfinite(v));
}
