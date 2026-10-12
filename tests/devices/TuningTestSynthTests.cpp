#include "core/RealtimeCheck.h"
#include "devices/test/TuningTestSynth.h"
#include "engine/MidiInputQueue.h"
#include "tuning/Tet12.h"
#include "support/AllocationHooks.h"
#include "tuning/TuningRegistry.h"

#include <juce_dsp/juce_dsp.h>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace lattice;

namespace
{
constexpr double sampleRate = 48000.0;

TimedMidi midi(int offset, std::uint8_t b0, std::uint8_t b1, std::uint8_t b2)
{
    TimedMidi e;
    e.sampleOffset = offset;
    e.bytes[0] = b0;
    e.bytes[1] = b1;
    e.bytes[2] = b2;
    e.size = 3;
    return e;
}

/** Renders a held note and returns the output after the attack. */
std::vector<float> renderNote(TuningTestSynth& synth, int note, int numSamples)
{
    const int block = 256;
    std::vector<float> out(static_cast<std::size_t>(numSamples + 4096));
    auto on = midi(0, 0x90, static_cast<std::uint8_t>(note), 100);
    for (std::size_t pos = 0; pos < out.size(); pos += block)
    {
        const int n = static_cast<int>(std::min<std::size_t>(block, out.size() - pos));
        synth.render(out.data() + pos, n, pos == 0 ? &on : nullptr, pos == 0 ? 1 : 0);
    }
    return {out.begin() + 4096, out.end()};
}

/** Strongest frequency within ±10% of expectedHz: Hann window, FFT, parabolic interpolation of the
    log-magnitude peak. Also returns the band peak relative to the whole spectrum's peak. */
std::pair<double, double> measureFundamental(const std::vector<float>& signal, double expectedHz)
{
    constexpr int order = 16;
    constexpr int size = 1 << order;
    REQUIRE(signal.size() >= static_cast<std::size_t>(size));

    std::vector<float> data(2 * size, 0.0f);
    for (int i = 0; i < size; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos(2.0 * juce::MathConstants<double>::pi * i / (size - 1));
        data[static_cast<std::size_t>(i)] = static_cast<float>(signal[static_cast<std::size_t>(i)] * w);
    }
    juce::dsp::FFT fft(order);
    fft.performFrequencyOnlyForwardTransform(data.data());

    const double binHz = sampleRate / size;
    const int lo = std::max(1, static_cast<int>(expectedHz * 0.9 / binHz));
    const int hi = std::min(size / 2 - 2, static_cast<int>(expectedHz * 1.1 / binHz) + 1);
    int peak = lo;
    for (int i = lo; i <= hi; ++i)
        if (data[static_cast<std::size_t>(i)] > data[static_cast<std::size_t>(peak)])
            peak = i;
    const float globalMax = *std::max_element(data.begin() + 1, data.begin() + size / 2);

    const double a = std::log(std::max(1e-12f, data[static_cast<std::size_t>(peak - 1)]));
    const double b = std::log(std::max(1e-12f, data[static_cast<std::size_t>(peak)]));
    const double c = std::log(std::max(1e-12f, data[static_cast<std::size_t>(peak + 1)]));
    const double offset = 0.5 * (a - c) / (a - 2.0 * b + c);
    return {(peak + offset) * binHz, data[static_cast<std::size_t>(peak)] / globalMax};
}
} // namespace

TEST_CASE("Test synth plays Colundi table frequencies", "[devices][tuning]")
{
    const auto colundi = TuningRegistry::makeBuiltinColundi();
    TrackTuning tuning(colundi);
    tuning.setSampleRate(sampleRate);

    for (const int note : {0, 12, 47, 100})
    {
        TuningTestSynth synth(tuning);
        synth.prepare(sampleRate);
        const auto out = renderNote(synth, note, 1 << 16);
        const double expected = colundi->hz[static_cast<std::size_t>(note)];
        const auto [measured, relativeLevel] = measureFundamental(out, expected);
        INFO("note " << note << ": expected " << expected << " Hz");
        CHECK(measured == Catch::Approx(expected).margin(0.5));
        CHECK(relativeLevel > 0.5); // the fundamental dominates the spectrum
        CHECK(synth.lastNoteOn() == note);
    }
}

TEST_CASE("Test synth follows a tuning change on a held note", "[devices][tuning]")
{
    TrackTuning tuning(Tet12::make());
    tuning.setSampleRate(sampleRate);
    TuningTestSynth synth(tuning);
    synth.prepare(sampleRate);
    std::vector<float> scratch(256);
    auto on = midi(0, 0x90, 47, 100);
    synth.render(scratch.data(), 256, &on, 1);

    tuning.setTuning(TuningRegistry::makeBuiltinColundi());
    std::vector<float> out(1 << 16);
    for (std::size_t pos = 0; pos < out.size(); pos += 256)
        synth.render(out.data() + pos, 256, nullptr, 0);

    const auto [measured, level] = measureFundamental(out, 440.0); // Colundi #47
    CHECK(measured == Catch::Approx(440.0).margin(0.5));
    juce::ignoreUnused(level);
}

TEST_CASE("Test synth: muted high notes never start a voice", "[devices][tuning]")
{
    TrackTuning tuning(TuningRegistry::makeBuiltinColundi());
    tuning.setSampleRate(sampleRate);
    TuningTestSynth synth(tuning);
    synth.prepare(sampleRate);
    std::vector<float> out(256);
    auto on = midi(0, 0x90, 127, 100);
    synth.render(out.data(), 256, &on, 1);
    CHECK(synth.activeVoiceCount() == 0);
    CHECK(std::all_of(out.begin(), out.end(), [](float s) { return s == 0.0f; }));
}

TEST_CASE("Test synth applies events at their sample offsets", "[devices]")
{
    TrackTuning tuning(Tet12::make());
    tuning.setSampleRate(sampleRate);
    TuningTestSynth synth(tuning);
    synth.prepare(sampleRate);
    std::vector<float> out(512);
    auto on = midi(100, 0x90, 69, 127);
    synth.render(out.data(), 512, &on, 1);
    CHECK(std::all_of(out.begin(), out.begin() + 101, [](float s) { return s == 0.0f; }));
    CHECK(std::any_of(out.begin() + 101, out.end(), [](float s) { return s != 0.0f; }));
}

TEST_CASE("Test synth: 16 voices, oldest stolen, note-off releases", "[devices]")
{
    TrackTuning tuning(Tet12::make());
    tuning.setSampleRate(sampleRate);
    TuningTestSynth synth(tuning);
    synth.prepare(sampleRate);
    std::vector<float> out(64);
    std::vector<TimedMidi> ons;
    for (int n = 40; n < 60; ++n)
        ons.push_back(midi(0, 0x90, static_cast<std::uint8_t>(n), 100));
    synth.render(out.data(), 64, ons.data(), static_cast<int>(ons.size()));
    CHECK(synth.activeVoiceCount() == TuningTestSynth::numVoices);

    std::vector<TimedMidi> offs;
    for (int n = 40; n < 60; ++n)
        offs.push_back(midi(0, 0x80, static_cast<std::uint8_t>(n), 0));
    synth.render(out.data(), 64, offs.data(), static_cast<int>(offs.size()));
    std::vector<float> tail(48000);
    synth.render(tail.data(), static_cast<int>(tail.size()), nullptr, 0); // 1 s: longer than the release
    CHECK(synth.activeVoiceCount() == 0);
}

TEST_CASE("Test synth renders 10,000 blocks with MIDI and allocates nothing", "[devices][rt]")
{
    LATTICE_REQUIRE_ALLOCATION_HOOKS();
    TrackTuning tuning(TuningRegistry::makeBuiltinColundi());
    tuning.setSampleRate(sampleRate);
    TuningTestSynth synth(tuning);
    synth.prepare(sampleRate);
    std::vector<float> out(128);
    const auto before = rt::countersForThisThread();
    const auto violations = rt::violationCount();
    {
        const rt::ScopedAudioThread audioThread;
        for (int i = 0; i < 10000; ++i)
        {
            TimedMidi events[3] = {midi(0, 0x90, static_cast<std::uint8_t>(30 + i % 60), 90),
                                   midi(40, 0xe0, 0, static_cast<std::uint8_t>(i % 128)),
                                   midi(90, 0x80, static_cast<std::uint8_t>(30 + (i + 50) % 60), 0)};
            synth.render(out.data(), 128, events, 3);
        }
    }
    const auto after = rt::countersForThisThread();
    CHECK(after.allocations == before.allocations);
    CHECK(after.deallocations == before.deallocations);
    CHECK(rt::violationCount() == violations);
}

TEST_CASE("MIDI input queue carries short messages to the consumer", "[engine][midi]")
{
    MidiInputQueue queue;
    CHECK(queue.push(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100))));
    CHECK(queue.push(juce::MidiMessage::noteOff(2, 61)));
    CHECK(queue.push(juce::MidiMessage::pitchWheel(1, 9000)));
    const juce::uint8 sysex[] = {0x7e, 0x7f, 0x06, 0x01};
    CHECK_FALSE(queue.push(juce::MidiMessage::createSysExMessage(sysex, 4)));
    CHECK(queue.droppedCount() == 1);

    TimedMidi out[8];
    int n = 0;
    {
        const rt::ScopedAudioThread audioThread;
        n = queue.popInto(out, 8);
    }
    REQUIRE(n == 3);
    CHECK(out[0].bytes[0] == 0x90);
    CHECK(out[0].bytes[1] == 60);
    CHECK(out[1].bytes[0] == 0x81);
    CHECK(out[2].bytes[0] == 0xe0);
    CHECK(out[0].sampleOffset == 0);
    CHECK(queue.popInto(out, 8) == 0);
}

TEST_CASE("MIDI input queue drops when full instead of blocking", "[engine][midi]")
{
    MidiInputQueue queue;
    int accepted = 0;
    for (int i = 0; i < MidiInputQueue::capacity + 10; ++i)
        accepted += queue.push(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100))) ? 1 : 0;
    CHECK(accepted == MidiInputQueue::capacity - 1);
    CHECK(queue.droppedCount() == 11);
}
