// MIDI effects (docs/04-m3-devices.md, "Tests" 5).

#include "devices/dsp/SyncRates.h"
#include "devices/midi/Arpeggiator.h"
#include "devices/midi/Chord.h"
#include "support/DeviceHarness.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace lattice;
using namespace lattice::test;

namespace
{
using Event = DeviceHarness::Event;

std::vector<Event> noteOns(const DeviceHarness& h)
{
    std::vector<Event> out;
    for (const auto& e : h.outputEvents)
        if ((e.status & 0xf0) == 0x90 && e.data2 > 0)
            out.push_back(e);
    return out;
}

std::vector<Event> noteOffs(const DeviceHarness& h)
{
    std::vector<Event> out;
    for (const auto& e : h.outputEvents)
        if ((e.status & 0xf0) == 0x80 || ((e.status & 0xf0) == 0x90 && e.data2 == 0))
            out.push_back(e);
    return out;
}

/** The sample of the first sample at or after a beat (as the engine's transport rounds it). */
std::int64_t sampleOf(double beat, double bpm = 120.0)
{
    return static_cast<std::int64_t>(std::ceil(beat * DeviceHarness::sampleRate * 60.0 / bpm - 1e-5));
}
} // namespace

TEST_CASE("Chord: +3 and +7 steps on note 47 plays 47, 50 and 54", "[devices][midi]")
{
    ChordDevice chord;
    setParam(chord, "chord.shift1", 3.0f);
    setParam(chord, "chord.shift2", 7.0f);
    setParam(chord, "chord.velocity2", 0.5f);
    DeviceHarness h(chord);
    h.noteOn(100, 47, 100);
    h.noteOff(5000, 47);
    h.renderTo(6000);

    const auto ons = noteOns(h);
    REQUIRE(ons.size() == 3);
    CHECK(ons[0].data1 == 47);
    CHECK(ons[1].data1 == 50);
    CHECK(ons[2].data1 == 54);
    CHECK(ons[2].data2 == 50); // its own velocity: half
    for (const auto& e : ons)
        CHECK(e.sample == 100);
    // The note-off ends exactly what the note started, even though the shifts change meanwhile.
    const auto offs = noteOffs(h);
    REQUIRE(offs.size() == 3);
    for (const auto& e : offs)
        CHECK(e.sample == 5000);
}

TEST_CASE("Chord: changing the shifts while a note is held still ends the right notes", "[devices][midi]")
{
    ChordDevice chord;
    setParam(chord, "chord.shift1", 4.0f);
    DeviceHarness h(chord);
    h.noteOn(0, 60);
    h.renderTo(1000);
    setParam(chord, "chord.shift1", 9.0f);
    h.noteOff(2000, 60);
    h.renderTo(3000);
    const auto offs = noteOffs(h);
    REQUIRE(offs.size() == 2);
    CHECK(offs[1].data1 == 64);
}

TEST_CASE("Arpeggiator: note order and timing are exact at every sync rate", "[devices][midi]")
{
    for (int rateIndex = 0; rateIndex < static_cast<int>(dsp::SyncRates::rates.size()); ++rateIndex)
    {
        const double beats = dsp::SyncRates::beatsAt(rateIndex);
        INFO("rate " << dsp::SyncRates::rates[static_cast<std::size_t>(rateIndex)].name);
        ArpeggiatorDevice arp;
        setParam(arp, "arp.rate", static_cast<float>(rateIndex));
        setParam(arp, "arp.gate", 0.5f);
        DeviceHarness h(arp);
        h.noteOn(0, 67);
        h.noteOn(0, 60);
        h.noteOn(0, 64);
        const int steps = static_cast<int>(std::min(9.0, std::max(3.0, 64.0 / beats)));
        h.renderTo(sampleOf(beats * steps) + 1, 0); // random block sizes

        const auto ons = noteOns(h);
        REQUIRE(static_cast<int>(ons.size()) >= steps);
        const int expectedNotes[3] = {60, 64, 67};
        for (int k = 0; k < steps; ++k)
        {
            INFO("step " << k);
            CHECK(ons[static_cast<std::size_t>(k)].sample == sampleOf(k * beats));
            CHECK(ons[static_cast<std::size_t>(k)].data1 == expectedNotes[k % 3]);
        }
        // Each step ends half a step later.
        const auto offs = noteOffs(h);
        REQUIRE(!offs.empty());
        CHECK(offs[0].sample == sampleOf(beats * 0.5));
    }
}

TEST_CASE("Arpeggiator: styles, ranges and transpose in table steps", "[devices][midi]")
{
    const auto run = [](ArpeggiatorDevice::Style style, int ranges, int distance, int transpose, int steps) {
        ArpeggiatorDevice arp;
        setParam(arp, "arp.style", static_cast<float>(style));
        setParam(arp, "arp.range", static_cast<float>(ranges));
        setParam(arp, "arp.distance", static_cast<float>(distance));
        setParam(arp, "arp.transpose", static_cast<float>(transpose));
        setParam(arp, "arp.rate", static_cast<float>(dsp::SyncRates::indexOf("1/16")));
        DeviceHarness h(arp);
        h.noteOn(0, 64);
        h.noteOn(0, 60);
        h.noteOn(0, 67);
        h.renderTo(sampleOf(0.25 * steps), 0);
        std::vector<int> notes;
        for (const auto& e : noteOns(h))
            notes.push_back(e.data1);
        return notes;
    };
    using S = ArpeggiatorDevice::Style;
    CHECK(run(S::Up, 1, 12, 0, 6) == std::vector<int>{60, 64, 67, 60, 64, 67});
    CHECK(run(S::Down, 1, 12, 0, 4) == std::vector<int>{67, 64, 60, 67});
    CHECK(run(S::UpDown, 1, 12, 0, 6) == std::vector<int>{60, 64, 67, 64, 60, 64});
    CHECK(run(S::Played, 1, 12, 0, 4) == std::vector<int>{64, 60, 67, 64});
    // Two ranges 17 table steps apart (a Colundi "page"), transposed by 2 steps.
    CHECK(run(S::Up, 2, 17, 2, 7) == std::vector<int>{62, 66, 69, 79, 83, 86, 62});
    const auto chord = run(S::Chord, 1, 12, 0, 2);
    CHECK(chord == std::vector<int>{60, 64, 67, 60, 64, 67});
}

TEST_CASE("Arpeggiator: releasing the keys stops it, unless hold is on", "[devices][midi]")
{
    for (const bool hold : {false, true})
    {
        INFO((hold ? "hold" : "no hold"));
        ArpeggiatorDevice arp;
        setParam(arp, "arp.hold", hold ? 1.0f : 0.0f);
        DeviceHarness h(arp);
        h.noteOn(0, 60);
        h.noteOff(sampleOf(1.0) - 100, 60);
        h.renderTo(sampleOf(4.0));
        int late = 0;
        for (const auto& e : noteOns(h))
            if (e.sample > sampleOf(1.5))
                ++late;
        if (hold)
            CHECK(late > 4);
        else
            CHECK(late == 0);
        // Every note it started was ended.
        CHECK(noteOns(h).size() == noteOffs(h).size());
    }
}
