// Where devices may go in a chain (docs/04-m3-devices.md, "3f"): MIDI effects, then one
// instrument, then audio effects; audio-only chains on returns, the master and Audio Effect Racks.

#include "devices/ChainEditing.h"
#include "devices/effects/Delay.h"
#include "devices/effects/EqEight.h"
#include "devices/effects/Reverb.h"
#include "devices/instruments/Sampler.h"
#include "devices/instruments/Synth.h"
#include "devices/midi/Arpeggiator.h"
#include "devices/midi/Chord.h"
#include "devices/racks/Rack.h"
#include "devices/racks/RackEditing.h"
#include "support/TestDevices.h"

#include <catch2/catch_test_macros.hpp>

using namespace lattice;
using namespace lattice::test;

namespace
{
struct Fixture
{
    Fixture()
    {
        set.resetToDefault(2, 1);
        for (int t = 0; t < 2; ++t)
        {
            auto c = LiveSet::chainOf(set.track(t));
            while (c.getNumChildren() > 0)
                set.removeDevice(c.getChild(0));
        }
    }
    juce::ValueTree chain(int t = 0) { return LiveSet::chainOf(set.track(t)); }
    juce::ValueTree insert(const juce::ValueTree& c, const juce::String& type, int index = -1)
    {
        return chains::insert(set, c, type, index, testRegistry());
    }
    /** The chain's device types, in order. */
    static juce::StringArray types(const juce::ValueTree& c)
    {
        juce::StringArray out;
        for (const auto& d : c)
            out.add(d[ids::deviceType].toString());
        return out;
    }

    juce::UndoManager undo;
    LiveSet set{&undo};
};
} // namespace

TEST_CASE("Chain rules: MIDI effects, then the instrument, then audio effects, wherever they're dropped",
          "[devices][chains]")
{
    Fixture f;
    auto c = f.chain();
    f.insert(c, DelayDevice::typeId, 0);
    f.insert(c, SynthDevice::typeId, -1);        // after the delay? No: before audio effects
    f.insert(c, ArpeggiatorDevice::typeId, -1);  // at the end? No: before the instrument
    f.insert(c, EqEightDevice::typeId, 0);       // first? No: after the instrument
    f.insert(c, ChordDevice::typeId, 1);         // between the arp and the synth: allowed
    f.insert(c, ReverbDevice::typeId, 4);        // between the EQ and the delay: allowed
    CHECK(Fixture::types(c)
          == juce::StringArray{ArpeggiatorDevice::typeId, ChordDevice::typeId, SynthDevice::typeId,
                               EqEightDevice::typeId, ReverbDevice::typeId, DelayDevice::typeId});
}

TEST_CASE("Chain rules: a second instrument replaces the first, in one undo step", "[devices][chains]")
{
    Fixture f;
    auto c = f.chain();
    f.insert(c, ArpeggiatorDevice::typeId);
    f.insert(c, SynthDevice::typeId);
    f.insert(c, DelayDevice::typeId);
    f.undo.beginNewTransaction();
    f.insert(c, SamplerDevice::typeId, 0);
    CHECK(Fixture::types(c)
          == juce::StringArray{ArpeggiatorDevice::typeId, SamplerDevice::typeId, DelayDevice::typeId});
    // An Instrument Rack is an instrument too.
    f.undo.beginNewTransaction();
    f.insert(c, RackDevice::instrumentRackId);
    CHECK(Fixture::types(c)
          == juce::StringArray{ArpeggiatorDevice::typeId, RackDevice::instrumentRackId, DelayDevice::typeId});
    f.undo.undo();
    f.undo.undo();
    CHECK(Fixture::types(c)
          == juce::StringArray{ArpeggiatorDevice::typeId, SynthDevice::typeId, DelayDevice::typeId});
}

TEST_CASE("Chain rules: returns, the master and Audio Effect Racks take audio effects only", "[devices][chains]")
{
    Fixture f;
    REQUIRE(f.set.numReturns() > 0);
    for (const auto& c : {LiveSet::chainOf(f.set.returnTrack(0)), LiveSet::chainOf(f.set.master())})
    {
        CHECK_FALSE(f.insert(c, SynthDevice::typeId).isValid());
        CHECK_FALSE(f.insert(c, ArpeggiatorDevice::typeId).isValid());
        CHECK_FALSE(f.insert(c, RackDevice::drumRackId).isValid());
        CHECK(f.insert(c, ReverbDevice::typeId).isValid());
        CHECK(f.insert(c, RackDevice::effectRackId).isValid());
        CHECK_FALSE(chains::place(c, SynthDevice::typeId, -1, testRegistry()).reason.isEmpty());
    }
    auto fxRack = f.insert(f.chain(), RackDevice::effectRackId);
    auto inner = racks::devicesOf(racks::addChain(f.set, fxRack));
    CHECK_FALSE(chains::carriesMidi(inner));
    CHECK_FALSE(f.insert(inner, SynthDevice::typeId).isValid());
    CHECK(f.insert(inner, DelayDevice::typeId).isValid());

    // An Instrument Rack's chain is a full chain.
    auto instRack = f.insert(f.chain(1), RackDevice::instrumentRackId);
    auto instChain = racks::devicesOf(racks::addChain(f.set, instRack));
    CHECK(chains::carriesMidi(instChain));
    CHECK(f.insert(instChain, ChordDevice::typeId).isValid());
    CHECK(f.insert(instChain, SynthDevice::typeId).isValid());
}

TEST_CASE("Chain rules: moving keeps the order rules, within a chain and between chains", "[devices][chains]")
{
    Fixture f;
    auto c = f.chain();
    auto arp = f.insert(c, ArpeggiatorDevice::typeId);
    auto synth = f.insert(c, SynthDevice::typeId);
    auto eq = f.insert(c, EqEightDevice::typeId);
    auto delay = f.insert(c, DelayDevice::typeId);

    // The delay dragged to the front lands right after the instrument.
    CHECK(chains::move(f.set, delay, c, 0, testRegistry()));
    CHECK(Fixture::types(c)
          == juce::StringArray{ArpeggiatorDevice::typeId, SynthDevice::typeId, DelayDevice::typeId,
                               EqEightDevice::typeId});
    // The EQ dragged back to the end (an index counting itself).
    CHECK(chains::move(f.set, eq, c, 4, testRegistry()));
    CHECK(Fixture::types(c)
          == juce::StringArray{ArpeggiatorDevice::typeId, SynthDevice::typeId, DelayDevice::typeId,
                               EqEightDevice::typeId});
    CHECK(chains::move(f.set, eq, c, 2, testRegistry()));
    CHECK(Fixture::types(c)
          == juce::StringArray{ArpeggiatorDevice::typeId, SynthDevice::typeId, EqEightDevice::typeId,
                               DelayDevice::typeId});
    // The arp dragged to the end stays before the instrument.
    CHECK(chains::move(f.set, arp, c, -1, testRegistry()));
    CHECK(c.indexOf(arp) == 0);

    // To another track: the synth replaces nothing there, then the delay follows it.
    CHECK(chains::move(f.set, synth, f.chain(1), 0, testRegistry()));
    CHECK(chains::move(f.set, delay, f.chain(1), 0, testRegistry()));
    CHECK(Fixture::types(f.chain(1)) == juce::StringArray{SynthDevice::typeId, DelayDevice::typeId});
    // Not to the master: it's audio only.
    CHECK_FALSE(chains::move(f.set, arp, LiveSet::chainOf(f.set.master()), 0, testRegistry()));
    CHECK(c.indexOf(arp) == 0);
}

TEST_CASE("Chain rules: racks nest at most 4 deep and never inside themselves", "[devices][chains][racks]")
{
    Fixture f;
    auto c = f.chain();
    std::vector<juce::ValueTree> rackStack;
    for (int depth = 0; depth < RackDevice::maxDepth; ++depth)
    {
        CHECK(chains::depthOf(c) == depth);
        auto rack = f.insert(c, RackDevice::effectRackId);
        REQUIRE(rack.isValid());
        rackStack.push_back(rack);
        c = racks::devicesOf(racks::addChain(f.set, rack));
    }
    // A fifth level is refused; a plain device is fine.
    CHECK_FALSE(f.insert(c, RackDevice::effectRackId).isValid());
    CHECK(f.insert(c, DelayDevice::typeId).isValid());

    // The outermost rack can't go into its own chain.
    auto outerChain = racks::devicesOf(rackStack.front().getChildWithName(ids::rackChains).getChild(0));
    CHECK_FALSE(chains::move(f.set, rackStack.front(), outerChain, -1, testRegistry()));
    // A two-deep rack moved into a chain at depth 3 would reach 5.
    auto deepChain = racks::devicesOf(rackStack[2].getChildWithName(ids::rackChains).getChild(0));
    auto twoDeep = f.insert(f.chain(1), RackDevice::effectRackId);
    f.insert(racks::devicesOf(racks::addChain(f.set, twoDeep)), RackDevice::effectRackId);
    CHECK(chains::depthOf(deepChain) == 3);
    CHECK_FALSE(chains::move(f.set, twoDeep, deepChain, -1, testRegistry()));
    CHECK(chains::move(f.set, twoDeep, racks::devicesOf(rackStack[1].getChildWithName(ids::rackChains).getChild(0)),
                       -1, testRegistry()));
}

TEST_CASE("Sidechain sources: never the device's own track, and never a loop", "[devices][chains][sidechain]")
{
    Fixture f;
    f.set.addTrack();
    auto c = [&](int t) { return LiveSet::chainOf(f.set.track(t)); };
    const auto id = [&](int t) { return f.set.trackId(t); };
    auto compA = f.insert(c(0), "lattice.compressor");
    auto compB = f.insert(c(1), "lattice.compressor");
    auto compC = f.insert(c(2), "lattice.compressor");

    CHECK(chains::sidechainSources(f.set, compA) == std::vector<std::int64_t>{id(1), id(2)});
    // B listens to A; then A may not listen to B (A -> B -> A), nor C once C listens to B.
    CHECK(chains::setSidechainSource(f.set, compB, id(0), false));
    CHECK(chains::sidechainSources(f.set, compA) == std::vector<std::int64_t>{id(2)});
    CHECK(chains::setSidechainSource(f.set, compC, id(1), true));
    CHECK(chains::sidechainSources(f.set, compA).empty());
    CHECK_FALSE(chains::setSidechainSource(f.set, compA, id(2), false));
    CHECK(static_cast<juce::int64>(compA.getChildWithName(ids::deviceState).getProperty(ids::sidechainTrack, 0)) == 0);
    // Clearing C's source frees track 3 for A again; a device on a return may listen to anything.
    CHECK(chains::setSidechainSource(f.set, compC, 0, false));
    CHECK(chains::setSidechainSource(f.set, compA, id(2), false));
    auto onReturn = f.insert(LiveSet::chainOf(f.set.returnTrack(0)), "lattice.compressor");
    CHECK(chains::sidechainSources(f.set, onReturn).size() == 3);

    // A listener inside a rack counts too.
    auto rack = f.insert(c(1), RackDevice::effectRackId);
    auto nested = f.insert(racks::devicesOf(racks::addChain(f.set, rack)), "lattice.compressor");
    CHECK(chains::setSidechainSource(f.set, compA, 0, false));
    CHECK(chains::setSidechainSource(f.set, compB, 0, false));
    CHECK(chains::setSidechainSource(f.set, nested, id(2), false));
    CHECK(chains::sidechainSources(f.set, compC) == std::vector<std::int64_t>{id(0)});
}

TEST_CASE("Chain rules: an effect inserted on a return starts fully wet", "[devices][chains]")
{
    juce::UndoManager undo;
    LiveSet set(&undo);
    set.resetToDefault(1, 1);
    REQUIRE(set.numReturns() >= 1);
    const auto& registry = testRegistry();
    const auto onReturn = chains::insert(set, LiveSet::chainOf(set.returnTrack(0)), ReverbDevice::typeId, -1, registry);
    REQUIRE(onReturn.isValid());
    CHECK(LiveSet::deviceParameter(onReturn, "reverb.mix", 0.0) == 1.0);
    // On a track it keeps its own default.
    const auto onTrack = chains::insert(set, LiveSet::chainOf(set.track(0)), ReverbDevice::typeId, -1, registry);
    REQUIRE(onTrack.isValid());
    CHECK(LiveSet::deviceParameter(onTrack, "reverb.mix", -1.0) == -1.0); // nothing stored: the default
}
