// Group tracks in the engine (docs/05-m4-ui.md, 4b): a group's chain runs on the mix of its members
// and its fader goes to its own parent; grouping alone changes nothing you hear; mute and solo
// follow the groups; delay compensation lines up paths through a group's latency, sends included;
// the output is the same with any number of worker threads; and a group's slot launches its row.

#include "devices/effects/Utility.h"
#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace lattice;
using namespace lattice::test;

namespace
{
/** Plain tracks with a recorder each (a steady 0.25 while a note is held), all armed. */
struct Fixture
{
    explicit Fixture(int numTracks, int workers = 0) : renderer(engine, BlockPattern::Fixed64)
    {
        engine.setWorkerThreads(workers);
        session.newSet(numTracks, 2);
        for (int t = 0; t < numTracks; ++t)
        {
            auto chain = LiveSet::chainOf(set().track(t));
            while (chain.getNumChildren() > 0)
                set().removeDevice(chain.getChild(0));
            set().insertDevice(chain, -1, TestRecorderDevice::typeId);
            set().setTrackArm(t, true, true);
        }
        session.flush();
        renderer.captureAudio = true;
    }

    LiveSet& set() { return session.set(); }
    juce::ValueTree add(int track, const juce::String& type) { return set().insertDevice(LiveSet::chainOf(set().track(track)), -1, type); }
    void noteOn() { engine.midiInput().push(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100))); }
    void render(std::int64_t samples)
    {
        session.flush();
        renderer.renderFor(samples);
        session.processEngineEvents();
    }
    float last() const { return renderer.audioLeft.back(); }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer;
};

std::size_t firstSound(const std::vector<float>& out, std::size_t from = 0)
{
    for (auto i = from; i < out.size(); ++i)
        if (out[i] != 0.0f)
            return i;
    return out.size();
}
} // namespace

TEST_CASE("Groups: grouping alone changes nothing you hear; a group's chain runs on its members' mix",
          "[engine][groups]")
{
    Fixture plain(3);
    plain.noteOn();
    plain.render(4800);

    Fixture grouped(3);
    grouped.set().groupTracks(0, 1); // G >A >B C
    grouped.noteOn();
    grouped.render(4800);
    REQUIRE(grouped.set().numTracks() == 4);
    CHECK(grouped.last() == Catch::Approx(plain.last()).epsilon(1e-6));

    // A gain on the group's chain scales A and B, not C.
    auto gain = grouped.add(0, TestGainDevice::typeId);
    grouped.set().setDeviceParameter(gain, "gain", 0.0);
    grouped.render(4800);
    CHECK(grouped.last() == Catch::Approx(plain.last() / 3.0f).epsilon(1e-5)); // only C is left
    // The group's fader too.
    grouped.set().setDeviceParameter(gain, "gain", 1.0);
    grouped.set().setTrackVolumeDb(0, LiveSet::minVolumeDb);
    grouped.render(48000);
    CHECK(grouped.last() == Catch::Approx(plain.last() / 3.0f).epsilon(1e-5));
}

TEST_CASE("Groups: nested groups each run on what's inside them", "[engine][groups]")
{
    Fixture f(4);
    f.set().groupTracks(0, 1);  // G >A >B C D
    f.set().groupTracks(0, 3);  // H >G >>A >>B >C D
    REQUIRE(f.set().groupDepth(2) == 2);
    f.noteOn();
    f.render(4800);
    const float all = f.last(); // four tracks
    auto inner = f.add(1, TestGainDevice::typeId); // on G: A and B
    f.set().setDeviceParameter(inner, "gain", 0.0);
    f.render(4800);
    CHECK(f.last() == Catch::Approx(all * 0.5f).epsilon(1e-5));
    auto outer = f.add(0, TestGainDevice::typeId); // on H: G and C
    f.set().setDeviceParameter(outer, "gain", 0.0);
    f.render(4800);
    CHECK(f.last() == Catch::Approx(all * 0.25f).epsilon(1e-5)); // only D
}

TEST_CASE("Groups: mute and solo follow the groups", "[engine][groups]")
{
    Fixture f(3);
    f.set().groupTracks(0, 1); // G >A >B C
    f.noteOn();
    f.render(4800);
    const float one = f.last() / 3.0f;

    f.set().setTrackMute(0, true); // the group: A and B
    f.render(4800);
    CHECK(f.last() == Catch::Approx(one).epsilon(1e-5));
    f.set().setTrackMute(0, false);

    f.set().setTrackSolo(1, true, false); // A: its group stays audible, B and C don't
    f.render(4800);
    CHECK(f.last() == Catch::Approx(one).epsilon(1e-5));

    f.set().setTrackSolo(0, true, false); // the group: A and B
    f.render(4800);
    CHECK(f.last() == Catch::Approx(2.0f * one).epsilon(1e-5));
    f.set().setTrackSolo(0, false, false);
    f.render(4800);
    CHECK(f.last() == Catch::Approx(3.0f * one).epsilon(1e-5));
}

TEST_CASE("Groups: delay compensation lines up a path through a group's latency, sends included",
          "[engine][groups]")
{
    // A in a group with a 100-sample delay on the group's chain; B outside it, with none.
    Fixture f(2);
    f.set().groupTracks(0, 0); // G >A B
    f.add(0, TestLatencyDevice::typeId);
    REQUIRE(f.set().numReturns() >= 1);
    f.render(4800);
    f.renderer.audioLeft.clear();

    f.noteOn();
    f.render(4800);
    const auto& out = f.renderer.audioLeft;
    const auto first = firstSound(out);
    REQUIRE(first < out.size());
    CHECK(first == static_cast<std::size_t>(TestLatencyDevice::latency)); // B waited for A's path
    CHECK(out[first] == Catch::Approx(out.back()).epsilon(1e-4));      // both arrive together

    // Only through the return (pre-fader sends; the tracks' own outputs off): A's send waits for its
    // group's latency, so the return still hears both at once.
    Fixture s(2);
    s.set().groupTracks(0, 0);
    s.add(0, TestLatencyDevice::typeId);
    for (const int t : {1, 2})
    {
        s.set().setTrackVolumeDb(t, LiveSet::minVolumeDb);
        s.set().setSendLevel(t, 0, 0.0);
    }
    s.set().setTrackVolumeDb(0, LiveSet::minVolumeDb);
    s.set().setReturnPreFader(0, true);
    s.render(4800);
    s.renderer.audioLeft.clear();
    s.noteOn();
    s.render(4800);
    const auto& viaReturn = s.renderer.audioLeft;
    const auto firstReturn = firstSound(viaReturn);
    REQUIRE(firstReturn < viaReturn.size());
    CHECK(firstReturn == static_cast<std::size_t>(TestLatencyDevice::latency));
    CHECK(viaReturn[firstReturn] == Catch::Approx(viaReturn.back()).epsilon(1e-4));
}

TEST_CASE("Groups: nested groups render the same with any number of worker threads", "[engine][groups]")
{
    std::vector<float> reference;
    for (const int workers : {0, 1, 3})
    {
        CAPTURE(workers);
        Fixture f(6, workers);
        REQUIRE(f.set().groupTracks(0, 1) == 0); // G >A >B C D E F
        REQUIRE(f.set().groupTracks(3, 4) == 3); // G >A >B K >C >D E F
        REQUIRE(f.set().groupTracks(0, 5) == 0); // H >G >>A >>B >K >>C >>D E F
        REQUIRE(f.set().groupDepth(5) == 2);
        f.add(1, TestLatencyDevice::typeId);     // on G
        auto u = f.add(4, UtilityDevice::typeId); // on K
        f.set().setDeviceParameter(u, "utility.gain", -6.0);
        f.set().setTrackPan(0, -0.4);            // H
        f.set().setSendLevel(3, 0, -6.0);        // B, inside two groups with latency in one
        f.noteOn();
        f.render(9600);
        if (reference.empty())
            reference = f.renderer.audioLeft;
        else
            CHECK(f.renderer.audioLeft == reference);
    }
}

TEST_CASE("Groups: a group's slot launches the clips in its row, stops them, and shows their state",
          "[engine][groups]")
{
    Fixture f(3);
    f.set().groupTracks(0, 1); // G >A >B C
    f.set().createClip(1, 0, 4.0);
    f.set().createClip(2, 0, 4.0);
    f.set().createClip(3, 0, 4.0); // C, outside the group
    f.render(480);
    CHECK(f.session.slotState(0, 0) == SlotState::Stopped);
    CHECK(f.session.slotState(0, 1) == SlotState::Empty);

    f.session.launchSlot(0, 0);
    f.render(48000 * 3);
    CHECK(f.session.slotState(1, 0) == SlotState::Playing);
    CHECK(f.session.slotState(2, 0) == SlotState::Playing);
    CHECK(f.session.slotState(3, 0) == SlotState::Stopped); // not in the group
    CHECK(f.session.slotState(0, 0) == SlotState::Playing);

    f.session.stopTrack(0);
    f.render(48000 * 3);
    CHECK(f.session.slotState(1, 0) == SlotState::Stopped);
    CHECK(f.session.slotState(0, 0) == SlotState::Stopped);
    // A group can't be armed: it has nothing to play.
    f.set().setTrackArm(0, true, false);
    CHECK(!static_cast<bool>(f.set().track(0)[ids::arm]));
}

TEST_CASE("Groups: grouping the first tracks of a running set keeps every member's fader in charge",
          "[engine][groups]")
{
    // The M4 4b hands-on checks: after grouping tracks 1 and 2, track 1's fader did nothing.
    Fixture f(3);
    f.noteOn();
    f.render(4800);
    const float all = f.last();
    REQUIRE(all != 0.0f);
    f.set().groupTracks(0, 1); // G >A >B C
    f.render(4800);
    CHECK(f.last() == Catch::Approx(all).epsilon(1e-5));
    for (int member : {1, 2, 3})
    {
        f.set().setTrackVolumeDb(member, LiveSet::minVolumeDb);
        f.render(4800);
        INFO("track " << member);
        CHECK(f.last() == Catch::Approx(all * 2.0f / 3.0f).epsilon(1e-3));
        f.set().setTrackVolumeDb(member, 0.0);
        f.render(4800);
    }
}
