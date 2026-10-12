// Session engine tests (docs/03-m2-engine-session.md, "Tests"). Every scenario that can run at
// several block sizes does, and must produce exactly the same events each time.

#include "support/AllocationHooks.h"
#include "support/OfflineRenderer.h"
#include "support/Spectrum.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <functional>

using namespace lattice;
using namespace lattice::test;

namespace
{
std::int64_t at(double beat, double bpm = 120.0)
{
    return OfflineRenderer::samplesForBeats(beat, bpm);
}

NoteData note(int pitch, double start, double length, int velocity = 100)
{
    NoteData n;
    n.pitch = pitch;
    n.start = start;
    n.length = length;
    n.velocity = velocity;
    return n;
}

/** An engine, a session with numTracks unarmed tracks, and an OfflineRenderer. Checks on
    destruction that nothing allocated on the audio thread. */
struct Fixture
{
    explicit Fixture(BlockPattern pattern = BlockPattern::Fixed64, int numTracks = 2, int numScenes = 4)
        : renderer(engine, pattern)
    {
        session.newSet(numTracks, numScenes);
        for (int t = 0; t < numTracks; ++t)
            session.set().setTrackArm(t, false, true);
        session.flush();
        violationsBefore = rt::violationCount();
    }

    ~Fixture() { CHECK(rt::violationCount() == violationsBefore); }

    void attach() { renderer.attach(session); }
    std::int64_t trackId(int index) const { return session.set().trackId(index); }
    LiveSet& set() { return session.set(); }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings};
    OfflineRenderer renderer;
    std::uint64_t violationsBefore = 0;
};

std::vector<RecordedEvent> byTrack(std::vector<RecordedEvent> events)
{
    std::stable_sort(events.begin(), events.end(),
                     [](const RecordedEvent& a, const RecordedEvent& b) { return a.trackId < b.trackId; });
    return events;
}

/** Runs a scenario with every block pattern; the recorded events must be identical. */
void forEveryPattern(const std::function<void(Fixture&)>& scenario, int numTracks = 2, int numScenes = 4)
{
    std::vector<RecordedEvent> reference;
    bool first = true;
    for (const auto pattern : allBlockPatterns)
    {
        INFO(describe(pattern));
        Fixture f(pattern, numTracks, numScenes);
        scenario(f);
        CHECK(f.renderer.overflow == 0);
        auto events = byTrack(f.renderer.events);
        if (first)
        {
            reference = std::move(events);
            first = false;
        }
        else
        {
            CHECK(events == reference);
        }
    }
}

std::vector<std::int64_t> samplesOf(const std::vector<RecordedEvent>& events)
{
    std::vector<std::int64_t> result;
    result.reserve(events.size());
    for (const auto& e : events)
        result.push_back(e.sample);
    return result;
}

std::vector<int> pitchesOf(const std::vector<RecordedEvent>& events)
{
    std::vector<int> result;
    result.reserve(events.size());
    for (const auto& e : events)
        result.push_back(e.data1);
    return result;
}

std::vector<RecordedEvent> noteOffs(const OfflineRenderer& r, std::int64_t trackId, int pitch)
{
    std::vector<RecordedEvent> result;
    for (const auto& e : r.events)
        if (e.trackId == trackId && e.isNoteOff() && e.data1 == pitch)
            result.push_back(e);
    return result;
}
} // namespace

// --- 1. Transport ----------------------------------------------------------------------------------

TEST_CASE("Transport: 4 beats at 120 BPM and 48 kHz are exactly 96,000 samples", "[engine][transport]")
{
    Transport t;
    t.prepare(48000.0);
    t.start(0, 0.0);
    CHECK(t.sampleAtBeat(4.0) == 96000);
    CHECK(t.beatAt(96000) == 4.0);
    CHECK(t.positionOf(4.0).bar == 2);
    CHECK(t.positionOf(5.25).beat == 2);
    CHECK(t.positionOf(5.25).sixteenth == 2);

    forEveryPattern([](Fixture& f) {
        f.session.play();
        f.renderer.renderTo(96000);
        const auto state = f.session.transport();
        CHECK(state.playing);
        CHECK(state.beat == 4.0);
    });
}

TEST_CASE("Transport: a tempo change mid-render lands exactly where hand calculation says",
          "[engine][transport]")
{
    forEveryPattern([](Fixture& f) {
        f.session.play();
        f.renderer.renderTo(48000); // beat 2 at 120 BPM
        CHECK(f.session.transport().beat == 2.0);
        f.set().setTempo(90.0); // 32,000 samples per beat from here
        f.session.flush();
        f.renderer.renderTo(48000 + 64000);
        CHECK(f.session.transport().beat == 4.0);
        CHECK(f.session.transport().tempo == 90.0);
    });
}

TEST_CASE("Transport: no drift after an hour", "[engine][transport]")
{
    Transport t;
    t.prepare(48000.0);
    t.start(0, 0.0);
    CHECK(t.beatAt(172'800'000) == 7200.0);
    CHECK(t.sampleAtBeat(7200.0) == 172'800'000);

    t.setTempo(133.0, 1'000'000);
    const double beatAtChange = t.beatAt(1'000'000);
    const double spb = 60.0 * 48000.0 / 133.0;
    CHECK(t.beatAt(1'000'000 + 172'800'000) ==
          Catch::Approx(beatAtChange + 172'800'000 / spb).epsilon(1e-15));
}

TEST_CASE("Transport: an hour rendered through the engine ends on the exact beat",
          "[engine][transport][slow]")
{
    Fixture f(BlockPattern::Random, 0, 1);
    f.session.play();
    f.renderer.renderTo(172'800'000);
    CHECK(f.session.transport().beat == 7200.0);
}

TEST_CASE("Transport: Stop, Continue and a second Stop", "[engine][transport]")
{
    Fixture f;
    f.session.play();
    f.renderer.renderTo(at(3.0));
    f.session.stop();
    f.renderer.renderFor(1000);
    CHECK_FALSE(f.session.transport().playing);
    CHECK(f.session.transport().beat == 3.0);

    f.session.continuePlayback();
    f.renderer.renderFor(at(1.0));
    CHECK(f.session.transport().beat == 4.0);

    f.session.stop();
    f.renderer.renderFor(100);
    f.session.stop(); // stopped already: back to bar 1
    f.renderer.renderFor(100);
    CHECK(f.session.transport().beat == 0.0);
}

// --- 2. Launch quantization -----------------------------------------------------------------------

TEST_CASE("Launch quantization: a launch at beat 1.3 starts on the next grid line", "[engine][launch]")
{
    struct Case
    {
        Quantization q;
        double beat;
    };
    for (const auto c :
         {Case{Quantization::None, 1.3}, Case{Quantization::Bars8, 32.0}, Case{Quantization::Bars4, 16.0},
          Case{Quantization::Bars2, 8.0}, Case{Quantization::Bar1, 4.0}, Case{Quantization::Half, 2.0},
          Case{Quantization::Quarter, 2.0}, Case{Quantization::Eighth, 1.5},
          Case{Quantization::Sixteenth, 1.5}})
    {
        INFO("quantization " << static_cast<int>(c.q));
        forEveryPattern([c](Fixture& f) {
            f.set().setGlobalQuantization(c.q);
            addClip(f.set(), 0, 0, 4.0, {note(60, 0.0, 0.5)});
            f.attach();
            f.session.play();
            f.renderer.renderTo(at(1.3)); // sample 31,200
            f.session.launchSlot(0, 0);
            CHECK(f.session.slotState(0, 0) == SlotState::Stopped); // not processed yet
            f.renderer.renderFor(1);
            if (c.q != Quantization::None)
                CHECK(f.session.slotState(0, 0) == SlotState::QueuedLaunch);
            f.renderer.renderTo(at(c.beat) + 100);
            const auto ons = f.renderer.noteOns(f.trackId(0));
            REQUIRE_FALSE(ons.empty());
            CHECK(ons.front().sample == at(c.beat));
            CHECK(f.session.slotState(0, 0) == SlotState::Playing);
        });
    }
}

TEST_CASE("Launch quantization: a clip's own value overrides the global one", "[engine][launch]")
{
    forEveryPattern([](Fixture& f) {
        f.set().setGlobalQuantization(Quantization::Bar1);
        auto clip = addClip(f.set(), 0, 0, 4.0, {note(60, 0.0, 0.5)});
        f.set().setClipProperty(clip, ids::quantization, static_cast<int>(Quantization::Quarter));
        f.attach();
        f.session.play();
        f.renderer.renderTo(at(1.3));
        f.session.launchSlot(0, 0);
        f.renderer.renderTo(at(5.0));
        const auto ons = f.renderer.noteOns(f.trackId(0));
        REQUIRE_FALSE(ons.empty());
        CHECK(ons.front().sample == at(2.0));
    });
}

TEST_CASE("Launching while stopped starts the transport and the clip at once", "[engine][launch]")
{
    forEveryPattern([](Fixture& f) {
        addClip(f.set(), 0, 0, 4.0, {note(60, 0.0, 0.5)});
        f.attach();
        f.renderer.renderTo(1000); // the engine has been running; the transport hasn't
        f.session.launchSlot(0, 0);
        f.renderer.renderTo(5000);
        const auto ons = f.renderer.noteOns(f.trackId(0));
        REQUIRE(ons.size() == 1);
        CHECK(ons[0].sample == 1000);
        CHECK(f.session.transport().playing);
        CHECK(f.session.transport().beat == Catch::Approx(4000.0 / 24000.0));
    });
}

// --- 3. Note timing --------------------------------------------------------------------------------

TEST_CASE("Note timing: notes land on their exact samples, across loop wraps", "[engine][timing]")
{
    forEveryPattern([](Fixture& f) {
        addClip(f.set(), 0, 0, 1.0, {note(60, 0.0, 0.25), note(62, 0.5, 0.25)});
        f.attach();
        f.session.launchSlot(0, 0); // starts at sample 0
        f.renderer.renderTo(at(4.0) + 10);

        const auto ons = f.renderer.noteOns(f.trackId(0));
        REQUIRE(ons.size() == 9);
        for (std::size_t k = 0; k < ons.size(); ++k)
        {
            CHECK(ons[k].sample == static_cast<std::int64_t>(k) * 12000);
            CHECK(ons[k].data1 == (k % 2 == 0 ? 60 : 62));
        }
        const auto offs = noteOffs(f.renderer, f.trackId(0), 62);
        REQUIRE(offs.size() == 4);
        CHECK(offs[0].sample == 18000);
        CHECK(offs[3].sample == 3 * 24000 + 18000);
    });
}

TEST_CASE("Note timing: a loop shorter than a block wraps several times within it", "[engine][timing]")
{
    forEveryPattern([](Fixture& f) {
        f.set().setTempo(999.0);
        addClip(f.set(), 0, 0, LiveSet::minLoopLength, {note(60, 0.0, LiveSet::minLoopLength / 2)});
        f.attach();
        f.session.launchSlot(0, 0);
        f.renderer.renderTo(48000);

        Transport reference;
        reference.prepare(48000.0);
        reference.setTempo(999.0, 0);
        reference.start(0, 0.0);
        std::vector<std::int64_t> expected;
        for (int k = 0;; ++k)
        {
            const auto s = reference.sampleAtBeat(k * LiveSet::minLoopLength);
            if (s >= 48000)
                break;
            expected.push_back(s);
        }
        CHECK(expected.size() > 200); // ~180 samples per loop: several loops in a 512-sample block
        CHECK(samplesOf(f.renderer.noteOns(f.trackId(0))) == expected);
        CHECK(f.renderer.unbalancedNotes(f.trackId(0)).size() <= 1); // the last note may still sound
    });
}

TEST_CASE("Note timing: a start marker and a loop region", "[engine][timing]")
{
    forEveryPattern([](Fixture& f) {
        // Plays 1..4 once (prelude), then loops 2..4.
        auto clip = addClip(f.set(), 0, 0, 4.0,
                            {note(60, 0.0, 0.5), note(61, 1.0, 0.5), note(62, 2.0, 0.5), note(63, 3.0, 0.5)});
        f.set().setClipProperty(clip, ids::startMarker, 1.0);
        f.set().setClipLoop(clip, 2.0, 4.0);
        f.attach();
        f.session.launchSlot(0, 0);
        f.renderer.renderTo(at(7.0) + 10);
        const auto ons = f.renderer.noteOns(f.trackId(0));
        CHECK(pitchesOf(ons) == std::vector<int>{61, 62, 63, 62, 63, 62, 63, 62});
        CHECK(samplesOf(ons) ==
              std::vector<std::int64_t>{0, at(1), at(2), at(3), at(4), at(5), at(6), at(7)});
    });
}

TEST_CASE("Note timing: a non-looping clip plays once and stops", "[engine][timing]")
{
    forEveryPattern([](Fixture& f) {
        auto clip = addClip(f.set(), 0, 0, 2.0, {note(60, 0.0, 0.5), note(61, 1.0, 4.0)});
        f.set().setClipProperty(clip, ids::looping, false);
        f.attach();
        f.session.launchSlot(0, 0);
        f.renderer.renderTo(at(6.0));
        CHECK(pitchesOf(f.renderer.noteOns(f.trackId(0))) == std::vector<int>{60, 61});
        const auto offs = noteOffs(f.renderer, f.trackId(0), 61);
        REQUIRE(offs.size() == 1);
        CHECK(offs[0].sample == at(2.0)); // cut at the clip end
        CHECK(f.session.slotState(0, 0) == SlotState::Stopped);
    });
}

// --- 4. No stuck notes -----------------------------------------------------------------------------

TEST_CASE("No stuck notes", "[engine][notes]")
{
    const auto setUp = [](Fixture& f) {
        f.set().setGlobalQuantization(Quantization::None);
        addClip(f.set(), 0, 0, 4.0, {note(60, 0.0, 3.0)});
        addClip(f.set(), 0, 1, 4.0, {note(64, 0.0, 3.0)});
        addClip(f.set(), 1, 0, 4.0, {note(67, 0.0, 3.0)});
        f.attach();
        f.session.launchSlot(0, 0);
        f.session.launchSlot(1, 0);
        f.renderer.renderTo(at(1.0));
    };
    const auto noStuckNotes = [](Fixture& f) {
        CHECK(f.renderer.unbalancedNotes(f.trackId(0)).empty());
        CHECK(f.renderer.unbalancedNotes(f.trackId(1)).empty());
    };

    SECTION("stopping a clip in the middle of a note")
    {
        forEveryPattern([&](Fixture& f) {
            setUp(f);
            f.session.stopTrack(0);
            f.renderer.renderTo(at(2.0));
            const auto offs = noteOffs(f.renderer, f.trackId(0), 60);
            REQUIRE(offs.size() == 1);
            CHECK(offs[0].sample == at(1.0));
            CHECK(f.renderer.unbalancedNotes(f.trackId(0)).empty());
        });
    }
    SECTION("switching clips")
    {
        forEveryPattern([&](Fixture& f) {
            setUp(f);
            f.session.launchSlot(0, 1);
            f.renderer.renderTo(at(2.0));
            CHECK(noteOffs(f.renderer, f.trackId(0), 60).at(0).sample == at(1.0));
            CHECK(f.renderer.noteOns(f.trackId(0)).back().data1 == 64);
            f.session.stopAllClips();
            f.renderer.renderTo(at(3.0));
            noStuckNotes(f);
        });
    }
    SECTION("stop all clips")
    {
        forEveryPattern([&](Fixture& f) {
            setUp(f);
            f.session.stopAllClips();
            f.renderer.renderTo(at(2.0));
            noStuckNotes(f);
            CHECK(f.session.slotState(1, 0) == SlotState::Stopped);
        });
    }
    SECTION("stopping the transport")
    {
        forEveryPattern([&](Fixture& f) {
            setUp(f);
            f.session.stop();
            f.renderer.renderTo(at(2.0));
            noStuckNotes(f);
            CHECK_FALSE(f.session.transport().playing);
        });
    }
    SECTION("a snapshot swap that removes the playing clip")
    {
        forEveryPattern([&](Fixture& f) {
            setUp(f);
            f.set().deleteClip(0, 0);
            f.session.flush();
            f.renderer.renderTo(at(2.0));
            CHECK(noteOffs(f.renderer, f.trackId(0), 60).at(0).sample == at(1.0));
            CHECK(f.renderer.unbalancedNotes(f.trackId(0)).empty());
        });
    }
    SECTION("an edit that removes the sounding note")
    {
        forEveryPattern([&](Fixture& f) {
            setUp(f);
            auto clip = f.set().clip(0, 0);
            f.set().removeNote(clip, clip.getChildWithName(ids::notes).getChild(0));
            f.session.flush();
            f.renderer.renderTo(at(2.0));
            CHECK(noteOffs(f.renderer, f.trackId(0), 60).at(0).sample == at(1.0));
            CHECK(f.session.slotState(0, 0) == SlotState::Playing);
        });
    }
    SECTION("an edit elsewhere keeps the sounding note")
    {
        forEveryPattern([&](Fixture& f) {
            setUp(f);
            f.set().addNote(f.set().clip(0, 0), note(72, 3.5, 0.25));
            f.session.flush();
            f.renderer.renderTo(at(4.0) + 10);
            CHECK(noteOffs(f.renderer, f.trackId(0), 60).at(0).sample == at(3.0)); // its own end
            CHECK(pitchesOf(f.renderer.noteOns(f.trackId(0))) == std::vector<int>{60, 72, 60});
        });
    }
}

// --- 5. Launch modes ---------------------------------------------------------------------------------

TEST_CASE("Launch modes", "[engine][launch]")
{
    const auto beatClip = [](Fixture& f, LaunchMode mode) {
        auto clip = addClip(f.set(), 0, 0, 1.0, {note(60, 0.0, 0.5)});
        f.set().setClipProperty(clip, ids::launchMode, static_cast<int>(mode));
        f.attach();
        f.session.play();
    };

    SECTION("Gate: plays while held; the release is quantized")
    {
        forEveryPattern([&](Fixture& f) {
            beatClip(f, LaunchMode::Gate);
            f.renderer.renderTo(at(0.5));
            f.session.launchSlot(0, 0); // starts at beat 4
            f.renderer.renderTo(at(1.0));
            f.session.releaseSlot(0, 0); // released before it started: plays one bar
            f.renderer.renderTo(at(9.0));
            f.session.launchSlot(0, 0); // starts at 12
            f.renderer.renderTo(at(13.25));
            f.session.releaseSlot(0, 0); // stops at 16
            f.renderer.renderTo(at(18.0));
            CHECK(samplesOf(f.renderer.noteOns(f.trackId(0))) ==
                  std::vector<std::int64_t>{at(4), at(5), at(6), at(7), at(12), at(13), at(14), at(15)});
            CHECK(f.session.slotState(0, 0) == SlotState::Stopped);
        });
    }
    SECTION("Toggle: a second press stops the clip")
    {
        forEveryPattern([&](Fixture& f) {
            beatClip(f, LaunchMode::Toggle);
            f.renderer.renderTo(at(0.5));
            f.session.launchSlot(0, 0);
            f.renderer.renderTo(at(5.0));
            f.session.launchSlot(0, 0);
            f.renderer.renderFor(1);
            CHECK(f.session.slotState(0, 0) == SlotState::QueuedStop);
            f.renderer.renderTo(at(10.0));
            CHECK(samplesOf(f.renderer.noteOns(f.trackId(0))) ==
                  std::vector<std::int64_t>{at(4), at(5), at(6), at(7)});
        });
    }
    SECTION("Trigger: the release is ignored and a second press restarts")
    {
        forEveryPattern([&](Fixture& f) {
            auto clip = addClip(f.set(), 0, 0, 4.0, {note(60, 0.0, 0.5), note(62, 2.0, 0.5)});
            juce::ignoreUnused(clip);
            f.attach();
            f.session.play();
            f.session.launchSlot(0, 0);
            f.renderer.renderTo(at(1.0));
            f.session.releaseSlot(0, 0);
            f.renderer.renderTo(at(5.0));
            f.session.launchSlot(0, 0); // restarts at bar 3 (beat 8)
            f.renderer.renderTo(at(9.0));
            CHECK(samplesOf(f.renderer.noteOns(f.trackId(0))) ==
                  std::vector<std::int64_t>{at(0), at(2), at(4), at(6), at(8)});
        });
    }
    SECTION("Legato keeps the position")
    {
        forEveryPattern([&](Fixture& f) {
            f.set().setGlobalQuantization(Quantization::None);
            addClip(f.set(), 0, 0, 4.0,
                    {note(60, 0.0, 0.5), note(60, 1.0, 0.5), note(60, 2.0, 0.5), note(60, 3.0, 0.5)});
            auto b =
                addClip(f.set(), 0, 1, 4.0,
                        {note(70, 0.0, 0.5), note(71, 1.0, 0.5), note(72, 2.0, 0.5), note(73, 3.0, 0.5)});
            f.set().setClipProperty(b, ids::legato, true);
            f.attach();
            f.session.launchSlot(0, 0);
            f.renderer.renderTo(at(6.0));
            f.session.launchSlot(0, 1); // A is at position 2: B continues from its position 2
            f.renderer.renderTo(at(8.0) + 10);
            const auto ons = f.renderer.noteOns(f.trackId(0));
            REQUIRE(ons.size() >= 3);
            CHECK(pitchesOf({ons.end() - 3, ons.end()}) == std::vector<int>{72, 73, 70});
            CHECK((ons.end() - 3)->sample == at(6.0));
        });
    }
    SECTION("An empty slot's stop button stops the track; without one nothing happens")
    {
        forEveryPattern([&](Fixture& f) {
            addClip(f.set(), 0, 0, 1.0, {note(60, 0.0, 0.5)});
            f.set().setHasStopButton(0, 2, false);
            f.attach();
            f.session.launchSlot(0, 0);
            f.renderer.renderTo(at(1.5));
            f.session.launchSlot(0, 2); // no stop button
            f.renderer.renderTo(at(4.5));
            CHECK(f.session.slotState(0, 0) == SlotState::Playing);
            f.session.launchSlot(0, 3); // stop button: stops at bar 3 (beat 8)
            f.renderer.renderTo(at(9.0));
            CHECK(f.session.slotState(0, 0) == SlotState::Stopped);
            CHECK(f.renderer.noteOns(f.trackId(0)).back().sample == at(7.0));
        });
    }
    SECTION("A scene launch: empty slots with stop buttons stop their tracks")
    {
        forEveryPattern([&](Fixture& f) {
            addClip(f.set(), 0, 0, 1.0, {note(60, 0.0, 0.5)});
            addClip(f.set(), 1, 0, 1.0, {note(67, 0.0, 0.5)});
            addClip(f.set(), 0, 1, 1.0, {note(62, 0.0, 0.5)});
            f.attach();
            f.session.launchScene(0);
            f.renderer.renderTo(at(1.5));
            CHECK(f.session.slotState(1, 0) == SlotState::Playing);
            f.session.launchScene(1);
            f.renderer.renderTo(at(5.0));
            CHECK(f.session.slotState(0, 1) == SlotState::Playing);
            CHECK(f.session.slotState(1, 0) == SlotState::Stopped);
            CHECK(f.renderer.noteOns(f.trackId(1)).back().sample == at(3.0));
            CHECK(f.renderer.noteOns(f.trackId(0)).back().data1 == 62);
        });
    }
    SECTION("A scene's tempo and time signature take effect at its launch point")
    {
        forEveryPattern([&](Fixture& f) {
            addClip(f.set(), 0, 1, 4.0, {note(60, 0.0, 0.5)});
            f.set().setSceneTempo(1, 90.0);
            f.set().setSceneTimeSignature(1, 3, 4);
            f.attach();
            f.session.play();
            f.renderer.renderTo(at(1.0));
            f.session.launchScene(1); // at beat 4
            f.renderer.renderTo(at(4.0) + 2 * 32000);
            CHECK(f.session.transport().tempo == 90.0);
            CHECK(f.session.transport().beat == 6.0);
            CHECK(f.session.transport().timeSigNum == 3);
            f.session.processEngineEvents();
            CHECK(f.set().tempo() == 90.0);
            CHECK(f.set().timeSigNumerator() == 3);
        });
    }
}

TEST_CASE("Stop all clips is quantized; a second press stops at once", "[engine][launch]")
{
    forEveryPattern([](Fixture& f) {
        addClip(f.set(), 0, 0, 1.0, {note(60, 0.0, 0.5)});
        f.attach();
        f.session.launchSlot(0, 0);
        f.renderer.renderTo(at(1.25));
        f.session.stopAllClips();
        f.renderer.renderTo(at(2.25));
        CHECK(f.session.transport().stopAllQueued);
        CHECK(f.session.slotState(0, 0) == SlotState::QueuedStop);
        f.session.stopAllClips();
        f.renderer.renderTo(at(3.5));
        CHECK(f.session.slotState(0, 0) == SlotState::Stopped);
        CHECK(f.renderer.noteOns(f.trackId(0)).back().sample == at(2.0));
        CHECK(f.renderer.unbalancedNotes(f.trackId(0)).empty());
    });
}

// --- 6. Follow actions -------------------------------------------------------------------------------

TEST_CASE("Follow actions: Next walks the column and wraps", "[engine][follow]")
{
    forEveryPattern(
        [](Fixture& f) {
            for (int s = 0; s < 3; ++s)
            {
                auto clip = addClip(f.set(), 0, s, 4.0, {note(60 + s, 0.0, 0.5)});
                f.set().setClipProperty(clip, ids::followA, static_cast<int>(FollowAction::Next));
                f.set().setClipProperty(clip, ids::followTime, 4.0);
            }
            f.attach();
            f.session.launchSlot(0, 0);
            f.renderer.renderTo(at(24.0) - 1);
            CHECK(pitchesOf(f.renderer.noteOns(f.trackId(0))) == std::vector<int>{60, 61, 62, 60, 61, 62});
            CHECK(samplesOf(f.renderer.noteOns(f.trackId(0))) ==
                  std::vector<std::int64_t>{0, at(4), at(8), at(12), at(16), at(20)});
        },
        2, 5);
}

TEST_CASE("Follow actions: seeded random choices are repeatable", "[engine][follow]")
{
    const auto run = [](Fixture& f, FollowAction action) {
        for (int s = 0; s < 4; ++s)
        {
            auto clip = addClip(f.set(), 0, s, 1.0, {note(60 + s, 0.0, 0.5)});
            f.set().setClipProperty(clip, ids::followA, static_cast<int>(action));
            f.set().setClipProperty(clip, ids::followB, static_cast<int>(FollowAction::Stop));
            f.set().setClipProperty(clip, ids::followChanceA, 100.0);
            f.set().setClipProperty(clip, ids::followTime, 1.0);
        }
        f.attach();
        f.session.setRandomSeed(42);
        f.session.launchSlot(0, 0);
        f.renderer.renderTo(at(40.0) - 1);
        return pitchesOf(f.renderer.noteOns(f.trackId(0)));
    };

    SECTION("Other never repeats the same clip")
    {
        std::vector<int> first;
        forEveryPattern([&](Fixture& f) {
            const auto pitches = run(f, FollowAction::Other);
            REQUIRE(pitches.size() == 40);
            for (std::size_t i = 1; i < pitches.size(); ++i)
                CHECK(pitches[i] != pitches[i - 1]);
            if (first.empty())
                first = pitches;
            CHECK(pitches == first);
        });
        // Every clip gets played.
        for (int p = 60; p < 64; ++p)
            CHECK(std::count(first.begin(), first.end(), p) > 0);
    }
    SECTION("Any can repeat, and a different seed gives a different order")
    {
        Fixture a;
        const auto withSeed42 = run(a, FollowAction::Any);
        Fixture b;
        b.session.setRandomSeed(7);
        for (int s = 0; s < 4; ++s)
        {
            auto clip = addClip(b.set(), 0, s, 1.0, {note(60 + s, 0.0, 0.5)});
            b.set().setClipProperty(clip, ids::followA, static_cast<int>(FollowAction::Any));
            b.set().setClipProperty(clip, ids::followTime, 1.0);
        }
        b.attach();
        b.session.launchSlot(0, 0);
        b.renderer.renderTo(at(40.0) - 1);
        CHECK(pitchesOf(b.renderer.noteOns(b.trackId(0))) != withSeed42);
    }
    SECTION("chance A 0% always takes B")
    {
        forEveryPattern([](Fixture& f) {
            auto clip = addClip(f.set(), 0, 0, 1.0, {note(60, 0.0, 0.5)});
            f.set().setClipProperty(clip, ids::followA, static_cast<int>(FollowAction::PlayAgain));
            f.set().setClipProperty(clip, ids::followB, static_cast<int>(FollowAction::Stop));
            f.set().setClipProperty(clip, ids::followChanceA, 0.0);
            f.set().setClipProperty(clip, ids::followTime, 2.0);
            f.attach();
            f.session.launchSlot(0, 0);
            f.renderer.renderTo(at(6.0));
            CHECK(samplesOf(f.renderer.noteOns(f.trackId(0))) == std::vector<std::int64_t>{0, at(1)});
            CHECK(f.session.slotState(0, 0) == SlotState::Stopped);
        });
    }
}

// --- 7. Mixer ----------------------------------------------------------------------------------------

TEST_CASE("Mixer: constant-power pan law", "[engine][mixer]")
{
    const auto centre = constantPowerPan(0.0);
    CHECK(centre.first == Catch::Approx(std::sqrt(0.5)));
    CHECK(centre.second == Catch::Approx(std::sqrt(0.5)));
    CHECK(constantPowerPan(-1.0).first == Catch::Approx(1.0));
    CHECK(constantPowerPan(-1.0).second == Catch::Approx(0.0).margin(1e-7));
    CHECK(constantPowerPan(1.0).second == Catch::Approx(1.0));
    const auto half = constantPowerPan(0.5);
    CHECK(half.first == Catch::Approx(std::cos(3.0 * juce::MathConstants<double>::pi / 8.0)));
    CHECK(half.second == Catch::Approx(std::sin(3.0 * juce::MathConstants<double>::pi / 8.0)));
    CHECK(half.first * half.first + half.second * half.second == Catch::Approx(1.0)); // constant power
}

TEST_CASE("Mixer: mute, solo and pan in the engine", "[engine][mixer]")
{
    Fixture f(BlockPattern::Fixed512, 2, 2);
    addClip(f.set(), 0, 0, 4.0, {note(69, 0.0, 4.0)});
    addClip(f.set(), 1, 0, 4.0, {note(76, 0.0, 4.0)});
    f.attach();
    f.session.launchScene(0);
    f.renderer.renderTo(at(0.5));
    const auto settle = [&] {
        f.renderer.renderFor(4800); // longer than the 20 ms gain ramp
        f.session.takeTrackPeaks(0);
        f.session.takeTrackPeaks(1);
        f.renderer.renderFor(2400);
    };

    settle();
    CHECK(f.session.takeTrackPeaks(0)[0] > 0.02f);
    CHECK(f.session.takeTrackPeaks(1)[0] > 0.02f);

    SECTION("a muted track is silent while its clip keeps playing")
    {
        f.set().setTrackMute(0, true);
        f.session.flush();
        settle();
        CHECK(f.session.takeTrackPeaks(0)[0] == 0.0f);
        CHECK(f.session.takeTrackPeaks(1)[0] > 0.02f);
        CHECK(f.session.slotState(0, 0) == SlotState::Playing);
        const auto before = f.renderer.noteOns(f.trackId(0)).size();
        f.renderer.renderTo(at(4.5));
        CHECK(f.renderer.noteOns(f.trackId(0)).size() == before + 1); // it looped
    }
    SECTION("solo silences the other tracks")
    {
        f.set().setTrackSolo(1, true, false);
        f.session.flush();
        settle();
        CHECK(f.session.takeTrackPeaks(0)[0] == 0.0f);
        CHECK(f.session.takeTrackPeaks(1)[0] > 0.02f);
    }
    SECTION("hard-left pan silences the right channel")
    {
        f.set().setTrackPan(0, -1.0);
        f.set().setTrackMute(1, true);
        f.session.flush();
        settle();
        const auto peaks = f.session.takeTrackPeaks(0);
        CHECK(peaks[0] > 0.02f);
        CHECK(peaks[1] < 1.0e-6f);
    }
}

TEST_CASE("Mixer: volume changes ramp instead of clicking", "[engine][mixer]")
{
    Fixture f(BlockPattern::Fixed64, 1, 1);
    addClip(f.set(), 0, 0, 8.0, {note(81, 0.0, 8.0)}); // 880 Hz in 12-TET
    f.attach();
    f.renderer.captureAudio = true;
    f.session.launchSlot(0, 0);
    f.renderer.renderTo(48000);
    f.set().setTrackVolumeDb(0, LiveSet::minVolumeDb); // to silence
    f.session.flush();
    f.renderer.renderTo(48000 + 4800);

    const auto& out = f.renderer.audioLeft;
    const auto peakIn = [&](std::size_t from, std::size_t to) {
        float p = 0.0f;
        for (auto i = from; i < to; ++i)
            p = std::max(p, std::abs(out[i]));
        return p;
    };
    const float before = peakIn(48000 - 2400, 48000);
    REQUIRE(before > 0.02f);
    CHECK(peakIn(48000, 48000 + 96) > 0.5f * before); // 2 ms in: still near full level
    CHECK(peakIn(48000 + 480, 48000 + 576) < 0.75f * before);
    CHECK(peakIn(48000 + 1200, 48000 + 4800) == 0.0f); // done after the 20 ms ramp

    float largestStep = 0.0f;
    for (std::size_t i = 48000; i < 48000 + 1200; ++i)
        largestStep = std::max(largestStep, std::abs(out[i] - out[i - 1]));
    float largestStepBefore = 0.0f;
    for (std::size_t i = 48000 - 1200; i < 48000; ++i)
        largestStepBefore = std::max(largestStepBefore, std::abs(out[i] - out[i - 1]));
    CHECK(largestStep <= largestStepBefore * 1.05f); // no discontinuity
}

// --- 8. Recording ------------------------------------------------------------------------------------

TEST_CASE("Recording", "[engine][record]")
{
    const auto pushNote = [](Fixture& f, int pitch, double from, double to) {
        f.renderer.renderTo(at(from));
        f.engine.midiInput().push(juce::MidiMessage::noteOn(1, pitch, static_cast<juce::uint8>(100)));
        f.renderer.renderTo(at(to));
        f.engine.midiInput().push(juce::MidiMessage::noteOff(1, pitch));
    };

    SECTION("fixed length: notes land at their beats and the clip turns into playback")
    {
        forEveryPattern([&](Fixture& f) {
            f.set().setTrackArm(0, true, false);
            f.set().setFixedRecordBars(1);
            f.attach();
            f.session.play();
            f.renderer.renderTo(at(1.0));
            f.session.launchSlot(0, 1); // empty slot on an armed track: record from beat 4
            f.renderer.renderFor(1);
            CHECK(f.session.slotState(0, 1) == SlotState::QueuedRecord);
            pushNote(f, 64, 4.5, 5.0);
            CHECK(f.session.slotState(0, 1) == SlotState::Recording);
            pushNote(f, 67, 7.0, 7.75);
            f.renderer.renderTo(at(9.0));
            f.session.processEngineEvents();

            CHECK(f.session.slotState(0, 1) == SlotState::Playing);
            const auto notes = LiveSet::notesOf(f.set().clip(0, 1));
            REQUIRE(notes.size() == 2);
            CHECK(notes[0].pitch == 64);
            CHECK(notes[0].start == Catch::Approx(0.5));
            CHECK(notes[0].length == Catch::Approx(0.5));
            CHECK(notes[1].start == Catch::Approx(3.0));
            CHECK(notes[1].length == Catch::Approx(0.75));
            CHECK(static_cast<double>(f.set().clip(0, 1)[ids::loopEnd]) == 4.0);

            // One undo step removes the whole recording; redo brings it back.
            f.session.undoManager().undo();
            CHECK_FALSE(f.set().clip(0, 1).isValid());
            f.session.undoManager().redo();
            CHECK(LiveSet::notesOf(f.set().clip(0, 1)).size() == 2);
        });
    }
    SECTION("open length: stopping rounds the clip up to the next bar, and it plays back")
    {
        forEveryPattern([&](Fixture& f) {
            f.set().setTrackArm(0, true, false);
            f.set().setGlobalQuantization(Quantization::None);
            f.attach();
            f.session.launchSlot(0, 0); // stopped: transport and recording start now
            pushNote(f, 60, 1.0, 1.5);
            pushNote(f, 62, 5.0, 5.25);
            f.renderer.renderTo(at(6.0));
            f.session.launchSlot(0, 0); // ends the recording at beat 6: rounds up to 8 beats
            f.renderer.renderTo(at(6.5));
            f.session.processEngineEvents();
            CHECK(static_cast<double>(f.set().clip(0, 0)[ids::loopEnd]) == 8.0);
            CHECK(f.session.slotState(0, 0) == SlotState::Playing);
            CHECK(LiveSet::notesOf(f.set().clip(0, 0)).size() == 2);

            const auto before = f.renderer.noteOns(f.trackId(0)).size();
            f.renderer.renderTo(at(9.0) + 10);
            const auto ons = f.renderer.noteOns(f.trackId(0));
            REQUIRE(ons.size() == before + 1);
            CHECK(ons.back().data1 == 60); // the loop came round: the recorded note plays back
            CHECK(ons.back().sample == at(9.0));
        });
    }
    SECTION("open length with quantization ends on the bar")
    {
        forEveryPattern([&](Fixture& f) {
            f.set().setTrackArm(0, true, false);
            f.attach();
            f.session.launchSlot(0, 0);
            pushNote(f, 60, 0.5, 1.0);
            f.renderer.renderTo(at(5.0));
            f.session.launchSlot(0, 0); // ends at the next bar: beat 8
            f.renderer.renderTo(at(7.0));
            CHECK(f.session.slotState(0, 0) == SlotState::Recording);
            f.renderer.renderTo(at(8.5));
            f.session.processEngineEvents();
            CHECK(static_cast<double>(f.set().clip(0, 0)[ids::loopEnd]) == 8.0);
            CHECK(f.session.slotState(0, 0) == SlotState::Playing);
        });
    }
    SECTION("overdub adds to the playing clip")
    {
        forEveryPattern([&](Fixture& f) {
            f.set().setTrackArm(0, true, false);
            addClip(f.set(), 0, 0, 4.0, {note(60, 0.0, 0.5)});
            f.attach();
            f.session.launchSlot(0, 0);
            f.renderer.renderTo(at(1.0));
            f.session.setSessionRecord(true, 0);
            pushNote(f, 67, 6.0, 6.5); // clip position 2
            f.renderer.renderTo(at(7.0));
            f.session.setSessionRecord(false, 0);
            f.renderer.renderTo(at(7.5));
            f.session.processEngineEvents();
            auto notes = LiveSet::notesOf(f.set().clip(0, 0));
            REQUIRE(notes.size() == 2);
            CHECK(notes[1].pitch == 67);
            CHECK(notes[1].start == Catch::Approx(2.0));
            CHECK(notes[1].length == Catch::Approx(0.5));
            f.renderer.renderTo(at(10.0) + 10);
            CHECK(f.renderer.noteOns(f.trackId(0)).back().data1 == 67); // played on the next pass
            f.session.undoManager().undo();
            CHECK(LiveSet::notesOf(f.set().clip(0, 0)).size() == 1);
        });
    }
    SECTION("a queued recording that is cancelled leaves no clip")
    {
        Fixture f;
        f.set().setTrackArm(0, true, false);
        f.attach();
        f.session.play();
        f.renderer.renderTo(at(1.0));
        f.session.launchSlot(0, 2);
        f.renderer.renderFor(10);
        CHECK(f.session.slotState(0, 2) == SlotState::QueuedRecord);
        f.session.launchSlot(0, 2); // pressed again before it started
        f.renderer.renderFor(10);
        f.session.processEngineEvents();
        CHECK_FALSE(f.set().clip(0, 2).isValid());
    }
}

// --- 9. Colundi through the engine -----------------------------------------------------------------

TEST_CASE("Colundi: a clip on a Colundi track plays the table's frequencies", "[engine][tuning]")
{
    const auto colundi = TuningRegistry::makeBuiltinColundi();
    for (const int pitch : {12, 47, 100})
    {
        INFO("note " << pitch);
        Fixture f(BlockPattern::Fixed512, 1, 1);
        f.set().setTrackTuning(0, TuningRegistry::colundiId);
        addClip(f.set(), 0, 0, 16.0, {note(pitch, 0.0, 16.0)});
        f.attach();
        f.renderer.captureAudio = true;
        f.session.launchSlot(0, 0);
        f.renderer.renderTo(4800 + (1 << 16));

        const std::vector<float> signal(f.renderer.audioLeft.begin() + 4800, f.renderer.audioLeft.end());
        const double expected = colundi->hz[static_cast<std::size_t>(pitch)];
        const auto [measured, level] = measureFundamental(signal, expected);
        CHECK(measured == Catch::Approx(expected).margin(0.5));
        CHECK(level > 0.5);
    }
    CHECK(colundi->hz[12] == Catch::Approx(59.9).margin(0.05));
    CHECK(colundi->hz[47] == Catch::Approx(440.0));
}

// --- Real-time safety ----------------------------------------------------------------------------------

TEST_CASE("A playing session renders 10,000 blocks without allocating on the audio thread", "[engine][rt]")
{
    LATTICE_REQUIRE_ALLOCATION_HOOKS();
    Fixture f(BlockPattern::Random, 4, 4);
    f.set().setMetronomeOn(true);
    f.set().setTrackArm(0, true, false);
    for (int t = 0; t < 4; ++t)
        for (int s = 0; s < 4; ++s)
        {
            auto clip =
                addClip(f.set(), t, s, 2.0, {note(48 + t * 5 + s, 0.0, 0.5), note(60 + s, 1.0, 0.75)});
            f.set().setClipProperty(clip, ids::followA, static_cast<int>(FollowAction::Any));
            f.set().setClipProperty(clip, ids::followTime, 2.0);
        }
    f.attach();
    f.session.launchScene(0);
    f.session.setSessionRecord(true, 0);
    const auto violations = rt::violationCount();
    for (int block = 0; block < 100; ++block)
    {
        f.engine.midiInput().push(
            juce::MidiMessage::noteOn(1, 60 + block % 12, static_cast<juce::uint8>(90)));
        f.renderer.renderFor(256 * 50);
        f.engine.midiInput().push(juce::MidiMessage::noteOff(1, 60 + block % 12));
        f.session.processEngineEvents(); // overdubbed notes go into the clips: snapshots change
        f.session.snapshots().collectGarbage();
    }
    CHECK(rt::violationCount() == violations);
}
