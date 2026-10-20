// The engine state feed (docs/05-m4-ui.md, "Rules", 1 and 3): one read of the engine per display
// tick, and each listener told only about what changed among the things it asked for.

#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"
#include "ui/feed/EngineStateFeed.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace lattice;
using namespace lattice::ui;
using namespace lattice::test;
using Feed = EngineStateFeed;

namespace
{
struct Recorder final : Feed::Listener
{
    std::vector<std::uint32_t> calls;
    std::function<void()> during;
    void engineStateChanged(std::uint32_t aspects) override
    {
        calls.push_back(aspects);
        if (during)
            during();
    }
    std::uint32_t last() const { return calls.empty() ? 0u : calls.back(); }
};

struct Fixture
{
    Fixture()
    {
        session.newSet(2, 2);
        for (int t = 0; t < 2; ++t)
        {
            auto chain = LiveSet::chainOf(session.set().track(t));
            while (chain.getNumChildren() > 0)
                session.set().removeDevice(chain.getChild(0));
        }
        session.flush();
    }

    void render(std::int64_t samples)
    {
        session.flush();
        renderer.renderFor(samples);
        session.processEngineEvents();
    }
    void tick()
    {
        now += 1.0 / 60.0;
        feed.tick(now);
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer{engine, BlockPattern::Fixed64};
    Feed feed{session, engine};
    double now = 10.0;
};
} // namespace

TEST_CASE("Feed: the first tick tells everyone; a quiet tick tells no one", "[ui][feed]")
{
    Fixture f;
    Recorder all, meters;
    f.feed.addListener(&all, Feed::allAspects);
    f.feed.addListener(&meters, Feed::metersAspect);
    f.tick();
    REQUIRE(all.calls.size() == 1);
    CHECK((all.last() & Feed::transportAspect) != 0);
    CHECK((all.last() & Feed::slotsAspect) != 0);
    CHECK((all.last() & Feed::loadAspect) != 0);
    CHECK(f.feed.frameSeconds() == 0.0);

    f.tick();
    f.tick();
    CHECK(all.calls.size() == 1); // nothing moved
    CHECK(f.feed.frameSeconds() == Catch::Approx(1.0 / 60.0));
    CHECK(f.feed.ticks() == 3);
}

TEST_CASE("Feed: a launched clip changes its slot; the transport moves every tick while playing", "[ui][feed]")
{
    Fixture f;
    Recorder slots, meters;
    f.feed.addListener(&slots, Feed::slotsAspect | Feed::transportAspect);
    f.feed.addListener(&meters, Feed::metersAspect);
    f.session.set().createClip(1, 1, 4.0);
    f.render(480);
    f.tick();
    CHECK(f.feed.slot(1, 1) == SlotState::Stopped);
    CHECK(f.feed.slot(0, 0) == SlotState::Empty);
    slots.calls.clear();
    meters.calls.clear(); // the first tick tells everyone

    f.session.launchSlot(1, 1);
    f.render(48000 * 3);
    f.tick();
    REQUIRE(!slots.calls.empty());
    CHECK((slots.last() & Feed::slotsAspect) != 0);
    CHECK(f.feed.slot(1, 1) == SlotState::Playing);
    REQUIRE(f.feed.changedSlots().size() == 1);
    CHECK(f.feed.changedSlots()[0] == std::pair<int, int>(1, 1));
    CHECK(f.feed.trackPlay(1).playingClipId == LiveSet::idOf(f.session.set().clip(1, 1)));
    CHECK(f.feed.transport().playing);

    // Playing: the position moves, so the transport (and the clip progress) change each tick.
    const auto before = slots.calls.size();
    f.render(800);
    f.tick();
    CHECK(slots.calls.size() == before + 1);
    CHECK((slots.last() & Feed::transportAspect) != 0);
    CHECK(meters.calls.empty()); // silent tracks: the meters never moved
}

TEST_CASE("Feed: meters are told every tick while anything sounds, then once more", "[ui][feed]")
{
    Fixture f;
    f.session.set().insertDevice(LiveSet::chainOf(f.session.set().track(0)), -1, TestRecorderDevice::typeId);
    f.session.set().setTrackArm(0, true, false);
    Recorder meters;
    f.feed.addListener(&meters, Feed::metersAspect);
    f.render(480);
    f.tick();
    meters.calls.clear();

    f.engine.midiInput().push(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)));
    f.render(4800);
    f.tick();
    CHECK(meters.calls.size() == 1);
    CHECK(f.feed.trackPeaks(0)[0] > 0.0f);
    CHECK(f.feed.masterPeaks()[0] > 0.0f);
    CHECK(f.feed.trackPeaks(1)[0] == 0.0f);

    f.engine.midiInput().push(juce::MidiMessage::noteOff(1, 60));
    f.render(48000);
    f.tick(); // the last levels since the note ended (a release may still sound)
    f.render(48000);
    f.tick();
    const auto quiet = meters.calls.size();
    f.render(480);
    f.tick();
    f.tick();
    CHECK(meters.calls.size() == quiet); // silence: no more meter ticks
    CHECK(f.feed.trackPeaks(0)[0] == 0.0f);
}

TEST_CASE("Feed: a listener can come and go while the feed is telling", "[ui][feed]")
{
    Fixture f;
    Recorder a, b;
    a.during = [&] { f.feed.removeListener(&b); };
    f.feed.addListener(&a, Feed::allAspects);
    f.feed.addListener(&b, Feed::allAspects);
    f.tick();
    CHECK(a.calls.size() == 1);
    CHECK(b.calls.empty()); // removed before its turn

    Recorder c;
    a.during = [&] { f.feed.addListener(&c, Feed::allAspects); };
    f.session.set().createClip(0, 0, 4.0);
    f.render(480);
    f.tick();
    CHECK(c.calls.empty()); // added during this tick: told from the next one
    a.during = nullptr;
    f.feed.removeListener(&c);
}
