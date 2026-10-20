// Racks and the Drum Rack (docs/04-m3-devices.md, "Tests" 7 and 8).

#include "devices/framework/ParameterPager.h"
#include "devices/racks/Rack.h"
#include "devices/racks/RackEditing.h"
#include "support/AllocationHooks.h"
#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace lattice;
using namespace lattice::test;

namespace
{
/** One armed track with an empty chain, and an OfflineRenderer; notes come in as live MIDI. */
struct Fixture
{
    Fixture() : renderer(engine, BlockPattern::Fixed64)
    {
        session.newSet(1, 1);
        auto chain = LiveSet::chainOf(set().track(0));
        while (chain.getNumChildren() > 0)
            set().removeDevice(chain.getChild(0));
        set().setTrackArm(0, true, false);
        session.flush();
        renderer.captureAudio = true;
        violationsBefore = rt::violationCount();
    }
    ~Fixture() { CHECK(rt::violationCount() == violationsBefore); }

    LiveSet& set() { return session.set(); }
    juce::ValueTree trackChain() { return LiveSet::chainOf(set().track(0)); }
    template <typename T>
    T* instance(const juce::ValueTree& device)
    {
        session.flush();
        return dynamic_cast<T*>(session.snapshots().deviceFor(LiveSet::idOf(device)));
    }
    juce::ValueTree addTo(const juce::ValueTree& chainDevices, const juce::String& type)
    {
        return set().insertDevice(chainDevices, -1, type);
    }
    void note(int n, int velocity = 100)
    {
        engine.midiInput().push(juce::MidiMessage::noteOn(1, n, static_cast<juce::uint8>(velocity)));
    }
    void release(int n) { engine.midiInput().push(juce::MidiMessage::noteOff(1, n)); }
    void render(std::int64_t samples)
    {
        session.flush();
        renderer.renderFor(samples);
    }
    float level() const { return renderer.audioLeft.back(); }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer;
    std::uint64_t violationsBefore = 0;
};

/** The output level one held recorder note makes: 0.25 through the track's and master's
    constant-power pans. */
constexpr float oneNote = 0.25f * 0.70710678f * 0.70710678f;
} // namespace

TEST_CASE("Instrument Rack: key and velocity zones send notes to the right chains, with fades", "[devices][racks]")
{
    Fixture f;
    auto rack = racks::insertRack(f.set(), f.trackChain(), -1, RackDevice::instrumentRackId);
    auto low = racks::addChain(f.set(), rack, "low");
    auto high = racks::addChain(f.set(), rack, "high");
    auto soft = racks::addChain(f.set(), rack, "soft");
    racks::setKeyZone(f.set(), low, 0, 59);
    racks::setKeyZone(f.set(), high, 60, 127, 4, 0); // fades in over 60..63
    racks::setVelocityZone(f.set(), soft, 1, 63);
    const auto lowRec = f.addTo(racks::devicesOf(low), TestRecorderDevice::typeId);
    const auto highRec = f.addTo(racks::devicesOf(high), TestRecorderDevice::typeId);
    const auto softRec = f.addTo(racks::devicesOf(soft), TestRecorderDevice::typeId);
    f.render(4800);

    f.note(50, 100);
    f.note(61, 100);
    f.note(70, 40);
    f.render(4800);

    auto* l = f.instance<TestRecorderDevice>(lowRec);
    auto* h = f.instance<TestRecorderDevice>(highRec);
    auto* s = f.instance<TestRecorderDevice>(softRec);
    REQUIRE(l != nullptr);
    REQUIRE(h != nullptr);
    REQUIRE(s != nullptr);
    REQUIRE(l->count.load() == 1);
    CHECK(l->received[0].note == 50);
    CHECK(l->received[0].velocity == 100);
    REQUIRE(h->count.load() == 2);
    CHECK(h->received[0].note == 61);
    CHECK(h->received[0].velocity == 40); // 2 / 5 of the way into the fade
    CHECK(h->received[1].note == 70);
    CHECK(h->received[1].velocity == 40);
    REQUIRE(s->count.load() == 1); // only the soft note
    CHECK(s->received[0].note == 70);

    // Note-offs follow their note-ons, whatever the velocity zone says.
    f.release(70);
    f.render(4800);
    CHECK(s->held[70] == 0);
    CHECK(h->held[70] == 0);
}

TEST_CASE("Macros move their parameters within their ranges and along their curves", "[devices][racks]")
{
    Fixture f;
    auto rack = racks::insertRack(f.set(), f.trackChain(), -1, RackDevice::effectRackId);
    auto chain = racks::addChain(f.set(), rack);
    const auto gainNode = f.addTo(racks::devicesOf(chain), TestGainDevice::typeId);
    auto mapping = racks::addMapping(f.set(), rack, 0, gainNode, "gain", 0.25, 0.75);
    f.render(256);
    auto* gain = f.instance<TestGainDevice>(gainNode);
    REQUIRE(gain != nullptr);
    auto& p = gain->parameter(0); // -1..2

    const auto check = [&](double macro, float expected) {
        f.set().setDeviceParameter(rack, "macro1", macro);
        f.render(1024); // a changed mapping is a new layout: the track switches to it after a short fade
        CHECK(p.plain() == Catch::Approx(expected).margin(1e-5));
    };
    check(0.0, -1.0f + 3.0f * 0.25f);
    check(127.0, -1.0f + 3.0f * 0.75f);
    check(63.5, -1.0f + 3.0f * 0.5f);

    // A slow-start curve: x^4.
    mapping.setProperty(ids::curve, 1.0, nullptr);
    check(63.5, -1.0f + 3.0f * (0.25f + 0.5f * 0.0625f));

    // Without the mapping, the parameter has its own value again.
    f.set().setDeviceParameter(gainNode, "gain", 0.3);
    racks::removeMapping(f.set(), mapping);
    f.render(4800);
    CHECK(p.plain() == Catch::Approx(0.3f));
    CHECK_FALSE(p.isOverridden());
}

TEST_CASE("The macro count goes from 0 to 64; lowering it removes mappings, undo restores them", "[devices][racks]")
{
    Fixture f;
    auto rack = racks::insertRack(f.set(), f.trackChain(), -1, RackDevice::effectRackId);
    auto chain = racks::addChain(f.set(), rack);
    const auto gainNode = f.addTo(racks::devicesOf(chain), TestGainDevice::typeId);
    racks::setMacroCount(f.set(), rack, 64);
    for (const int m : {0, 9, 20, 40})
        racks::addMapping(f.set(), rack, m, gainNode, "gain");
    f.render(256);
    auto* device = f.instance<RackDevice>(rack);
    REQUIRE(device != nullptr);

    for (const int count : {0, 8, 16, 32, 64})
    {
        INFO(count << " macros");
        f.session.undoManager().beginNewTransaction();
        racks::setMacroCount(f.set(), rack, count);
        f.session.flush();
        CHECK(racks::macroCount(rack) == count);
        CHECK(device->macroCount() == count);
        // Controllers page only the macros in use.
        const auto pages = ParameterPager::pagesFor(*device, 8);
        CHECK(static_cast<int>(pages.size()) == std::max(1, count / 8));
        std::size_t paged = 0;
        for (const auto& page : pages)
            paged += page.parameters.size();
        CHECK(static_cast<int>(paged) == count);
        f.render(256);
        if (count != 64) // 64 is what it was: nothing to undo
            f.session.undoManager().undo(); // back to 64 macros and all four mappings
        CHECK(racks::numMappings(rack) == 4);
    }

    f.session.undoManager().beginNewTransaction();
    racks::setMacroCount(f.set(), rack, 16);
    CHECK(racks::numMappings(rack) == 2); // macros 21 and 41 are gone, and their mappings with them
    f.session.undoManager().undo();
    CHECK(racks::numMappings(rack) == 4);
    CHECK(racks::macroCount(rack) == 64);
}

TEST_CASE("Recalling a macro variation restores the exact values", "[devices][racks]")
{
    Fixture f;
    auto rack = racks::insertRack(f.set(), f.trackChain(), -1, RackDevice::instrumentRackId);
    juce::Random random(11);
    std::vector<double> stored;
    for (int m = 1; m <= 8; ++m)
    {
        stored.push_back(random.nextDouble() * 127.0);
        f.set().setDeviceParameter(rack, "macro" + juce::String(m), stored.back());
    }
    const int index = racks::storeVariation(f.set(), rack, "A");
    for (int m = 1; m <= 8; ++m)
        f.set().setDeviceParameter(rack, "macro" + juce::String(m), 0.0);
    racks::recallVariation(f.set(), rack, index);
    for (int m = 1; m <= 8; ++m)
        CHECK(LiveSet::deviceParameter(rack, "macro" + juce::String(m), -1.0) == stored[static_cast<std::size_t>(m - 1)]);
    f.render(256);
    auto* device = f.instance<RackDevice>(rack);
    CHECK(device->parameter(0).plain() == static_cast<float>(stored[0]));
}

TEST_CASE("Nested racks render, and chain mute and solo work", "[devices][racks]")
{
    Fixture f;
    auto outer = racks::insertRack(f.set(), f.trackChain(), -1, RackDevice::instrumentRackId);
    auto a = racks::addChain(f.set(), outer, "A");
    auto b = racks::addChain(f.set(), outer, "B");
    // A: a rack inside a rack inside the outer rack (three deep), holding a recorder.
    auto middle = racks::insertRack(f.set(), racks::devicesOf(a), -1, RackDevice::instrumentRackId);
    auto inner = racks::insertRack(f.set(), racks::devicesOf(racks::addChain(f.set(), middle)), -1,
                                   RackDevice::instrumentRackId);
    f.addTo(racks::devicesOf(racks::addChain(f.set(), inner)), TestRecorderDevice::typeId);
    f.addTo(racks::devicesOf(b), TestRecorderDevice::typeId);
    f.render(4800);

    f.note(60);
    f.render(4800);
    CHECK(f.level() == Catch::Approx(2.0f * oneNote).epsilon(1e-4)); // both chains, at unity

    racks::setChainMix(f.set(), b, 0.0, 0.0, true, false); // mute B
    f.render(4800);
    CHECK(f.level() == Catch::Approx(oneNote).epsilon(1e-4));

    racks::setChainMix(f.set(), b, 0.0, 0.0, false, true); // solo B
    f.render(4800);
    CHECK(f.level() == Catch::Approx(oneNote).epsilon(1e-4));

    racks::setChainMix(f.set(), b, -6.0206, 0.0, false, false); // B at half level
    f.render(4800);
    CHECK(f.level() == Catch::Approx(1.5f * oneNote).epsilon(1e-3));
}

TEST_CASE("Drum Rack: pads by note, choke groups cut the other pads in the group", "[devices][racks]")
{
    Fixture f;
    auto drums = racks::insertRack(f.set(), f.trackChain(), -1, RackDevice::drumRackId);
    juce::ValueTree recorders[3];
    const int notes[3] = {36, 42, 38};
    for (int i = 0; i < 3; ++i)
    {
        auto pad = racks::addChain(f.set(), drums);
        pad.setProperty(ids::receiveNote, notes[i], nullptr);
        recorders[i] = f.addTo(racks::devicesOf(pad), TestRecorderDevice::typeId);
        if (i < 2)
            racks::setChokeGroup(f.set(), pad, 1); // 36 and 42 choke each other
    }
    f.render(4800);
    auto* kick = f.instance<TestRecorderDevice>(recorders[0]);
    auto* hat = f.instance<TestRecorderDevice>(recorders[1]);
    auto* snare = f.instance<TestRecorderDevice>(recorders[2]);

    f.note(36);
    f.note(38);
    f.render(4800);
    REQUIRE(kick->count.load() == 1);
    CHECK(kick->received[0].note == 60); // pads play their play note (C3) by default
    CHECK(snare->count.load() == 1);
    CHECK(hat->count.load() == 0);

    f.note(42); // in 36's choke group
    f.render(4800);
    CHECK(hat->count.load() == 1);
    CHECK(kick->chokes.load() >= 1);
    CHECK(kick->held[60] == 0);
    CHECK(snare->chokes.load() == 0);
    CHECK(snare->held[60] == 1);

    // Tuned pads: the pad's own note goes through (so the track tuning applies to it).
    racks::setTunedPads(f.set(), drums, true);
    f.render(4800);
    f.note(38);
    f.render(4800);
    REQUIRE(snare->count.load() == 2);
    CHECK(snare->received[1].note == 38);
}

TEST_CASE("Inside a rack, chains with different latencies line up", "[devices][racks][latency]")
{
    Fixture f;
    f.addTo(f.trackChain(), TestRecorderDevice::typeId); // a step when the note starts
    auto rack = racks::insertRack(f.set(), f.trackChain(), -1, RackDevice::effectRackId);
    f.addTo(racks::devicesOf(racks::addChain(f.set(), rack)), TestLatencyDevice::typeId);
    racks::addChain(f.set(), rack); // no latency
    f.render(4800);
    REQUIRE(f.session.processorForTrack(0)->chain()->latencySamples == TestLatencyDevice::latency);

    f.renderer.audioLeft.clear();
    f.note(60);
    f.render(4800);
    // Without compensation the output would step to half first, then to full 100 samples later.
    const auto& out = f.renderer.audioLeft;
    std::size_t first = 0;
    while (first < out.size() && out[first] == 0.0f)
        ++first;
    REQUIRE(first < out.size());
    const float full = 2.0f * oneNote;
    CHECK(out[first] == Catch::Approx(full).epsilon(1e-4));
    CHECK(out.back() == Catch::Approx(full).epsilon(1e-4));
}
