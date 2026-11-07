// Several surfaces and MPE (docs/06-m5-controllers.md, 5g; Tests 5 and 6): pads playing per note,
// each note on a channel of its own with its slide, timbre and pressure; live input meant for one
// track; and Push, Virtual Push and a Launchpad at once, following the window's selection
// together, or one of them keeping its own and playing its own track.

#include "support/FakeSurface.h"
#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"
#include "control/modes/NoteMode.h"
#include "control/modes/SessionMode.h"
#include "control/surface/PadPlayer.h"
#include "control/surface/Presets.h"
#include "control/surface/SurfaceController.h"
#include "ui/commands/SessionCommands.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace lattice;
using namespace lattice::control;
using namespace lattice::test;
using Kind = ui::SessionSelection::Kind;

namespace
{
std::vector<TimedMidi> drain(AudioEngine& engine)
{
    std::vector<TimedMidi> out(MidiInputQueue::capacity);
    int n = 0;
    {
        const rt::ScopedAudioThread audio;
        n = engine.midiInput().popInto(out.data(), static_cast<int>(out.size()));
    }
    out.resize(static_cast<std::size_t>(n));
    return out;
}

int status(const TimedMidi& e) { return e.bytes[0] & 0xf0; }
int channelOf(const TimedMidi& e) { return e.bytes[0] & 0x0f; }
bool flagged(const TimedMidi& e) { return (e.flags & TimedMidi::perNote) != 0; }
int bendOf(const TimedMidi& e) { return e.bytes[1] | (e.bytes[2] << 7); }
int bendFor(double steps) { return 8192 + static_cast<int>(std::lround(steps / perNoteBendSteps * 8191.0)); }

/** A 4x4 Virtual Push's map: row 0 a melodic row (60 62 64 65, as In Key's), row 1 drums (no
    slide), the rest unused. */
std::shared_ptr<const PadNoteMap> melodicMap(const SurfaceCapabilities& caps, Expression e = {})
{
    auto m = PadNoteMap::empty(caps);
    m.setExpression(e, caps);
    m.source = 9;
    const int notes[]{60, 62, 64, 65};
    for (int c = 0; c < 4; ++c)
    {
        auto& p = m.pad(0, c);
        p.kind = PadNoteMap::Pad::Kind::note;
        p.note = static_cast<std::uint8_t>(notes[c]);
        p.slides = true;
        auto& d = m.pad(1, c);
        d.kind = PadNoteMap::Pad::Kind::note;
        d.note = static_cast<std::uint8_t>(36 + c);
    }
    return std::make_shared<const PadNoteMap>(std::move(m));
}

void play(PadPlayer& p, SurfaceInput in)
{
    p.handle(in);
}
} // namespace

TEST_CASE("PadPlayer per note (MPE): each note on a channel of its own, at rest before it starts", "[control][fastpath][mpe]")
{
    TuningRegistry tunings;
    AudioEngine engine;
    const auto caps = presets::virtualPush(4, 4, 0);
    REQUIRE(caps.aftertouch == Aftertouch::mpe);
    PadPlayer player(engine, caps);
    player.publish(melodicMap(caps));
    REQUIRE(player.published()->perNote);

    play(player, SurfaceInput::padDown(0, 0, 1.0f));
    play(player, SurfaceInput::padDown(0, 1, 1.0f));
    const auto got = drain(engine);
    // Each: its bend at the middle, its timbre at 64 and its pressure at 0, then the note.
    REQUIRE(got.size() == 8);
    for (int n = 0; n < 2; ++n)
    {
        const auto* e = &got[static_cast<std::size_t>(n * 4)];
        const int ch = channelOf(e[0]);
        CHECK(ch != 0); // the master channel is the strip's
        for (int i = 0; i < 4; ++i)
        {
            CHECK(channelOf(e[i]) == ch);
            CHECK(flagged(e[i]));
            CHECK(e[i].source == 9);
        }
        CHECK(status(e[0]) == 0xe0);
        CHECK(bendOf(e[0]) == 8192);
        CHECK(status(e[1]) == 0xb0);
        CHECK(e[1].bytes[1] == 74);
        CHECK(e[1].bytes[2] == 64);
        CHECK(status(e[2]) == 0xd0);
        CHECK(e[2].bytes[1] == 0);
        CHECK(status(e[3]) == 0x90);
        CHECK(e[3].bytes[1] == (n == 0 ? 60 : 62));
    }
    CHECK(channelOf(got[0]) != channelOf(got[4]));
    CHECK(player.heldNotes() == 2);

    SECTION("Two pads of the same note are two voices, each ending with its own release")
    {
        auto m = *melodicMap(caps);
        m.pad(2, 0) = m.pad(0, 0);
        player.publish(std::make_shared<const PadNoteMap>(m));
        play(player, SurfaceInput::padDown(2, 0, 1.0f));
        const auto again = drain(engine);
        REQUIRE(again.size() == 4);
        CHECK(status(again[3]) == 0x90); // no note-off of the first: it plays on
        play(player, SurfaceInput::padUp(0, 0));
        const auto off = drain(engine);
        REQUIRE(off.size() == 1);
        CHECK(status(off[0]) == 0x80);
        CHECK(channelOf(off[0]) == channelOf(got[0]));
        CHECK(player.heldNotes() == 2);
    }

    SECTION("A note's channel: one holding nothing, used least recently; with all fifteen busy, the oldest's")
    {
        play(player, SurfaceInput::padUp(0, 0));
        drain(engine);
        // The first note's channel was used last (its release): a fresh one comes first.
        play(player, SurfaceInput::padDown(0, 2, 1.0f));
        const int third = channelOf(drain(engine)[0]);
        CHECK(third != channelOf(got[0]));
        CHECK(third != channelOf(got[4]));

        auto m = *melodicMap(caps);
        for (int i = 0; i < 16; ++i)
        {
            auto& p = m.pads[static_cast<std::size_t>(i)];
            p.kind = PadNoteMap::Pad::Kind::note;
            p.note = static_cast<std::uint8_t>(40 + i);
        }
        player.publish(std::make_shared<const PadNoteMap>(m));
        std::set<int> used{channelOf(got[4]), third};
        for (int i = 4; i < 16; ++i)
        {
            play(player, SurfaceInput::padDown(i / 4, i % 4, 1.0f));
            used.insert(channelOf(drain(engine)[0]));
        }
        // 2 held before, 12 more: 14 channels; the 15th free one next, then the oldest note's.
        CHECK(used.size() == 14);
        CHECK(used.count(0) == 0);
        play(player, SurfaceInput::padDown(0, 0, 1.0f));
        CHECK(used.count(channelOf(drain(engine)[0])) == 0);
        play(player, SurfaceInput::padDown(0, 3, 1.0f));
        CHECK(channelOf(drain(engine)[0]) == channelOf(got[4])); // the oldest held: pad (0, 1)'s
    }

    SECTION("Per-note expression off: one channel, polyphonic pressure, as before")
    {
        play(player, SurfaceInput::padUp(0, 0));
        play(player, SurfaceInput::padUp(0, 1));
        drain(engine);
        Expression off;
        off.perNote = false;
        player.publish(melodicMap(caps, off));
        play(player, SurfaceInput::padDown(0, 0, 1.0f));
        play(player, SurfaceInput::padPressure(0, 0, 0.5f));
        play(player, SurfaceInput::padSlide(0, 0, 1.0f));
        const auto plain = drain(engine);
        REQUIRE(plain.size() == 2);
        CHECK(plain[0].bytes[0] == 0x90);
        CHECK(plain[1].bytes[0] == 0xa0);
        CHECK_FALSE(flagged(plain[0]));
    }
}

TEST_CASE("PadPlayer per note: a slide follows the notes of the pads it crosses; timbre and pressure are the note's own",
          "[control][fastpath][mpe]")
{
    TuningRegistry tunings;
    AudioEngine engine;
    const auto caps = presets::virtualPush(4, 4, 0);
    PadPlayer player(engine, caps);
    player.publish(melodicMap(caps));
    play(player, SurfaceInput::padDown(0, 0, 1.0f));
    const int ch = channelOf(drain(engine)[0]);

    const auto slideTo = [&](float pads) {
        play(player, SurfaceInput::padSlide(0, 0, pads));
        const auto got = drain(engine);
        REQUIRE(got.size() == 1);
        CHECK(status(got[0]) == 0xe0);
        CHECK(channelOf(got[0]) == ch);
        CHECK(flagged(got[0]));
        return bendOf(got[0]);
    };
    CHECK(slideTo(1.0f) == bendFor(2.0));  // the next pad: 62
    CHECK(slideTo(1.5f) == bendFor(3.0));  // half way from 62 to 64
    CHECK(slideTo(3.0f) == bendFor(5.0));  // 65
    CHECK(slideTo(4.0f) == bendFor(6.0));  // past the row's end: its last step (1) carried on
    CHECK(slideTo(-1.0f) == bendFor(-1.0)); // before its start: a step down
    CHECK(slideTo(0.0f) == 8192);
    play(player, SurfaceInput::padSlide(0, 0, 0.0f));
    CHECK(drain(engine).empty()); // nothing new

    play(player, SurfaceInput::padTimbre(0, 0, 1.0f));
    play(player, SurfaceInput::padPressure(0, 0, 0.5f));
    const auto got = drain(engine);
    REQUIRE(got.size() == 2);
    CHECK(got[0].bytes[0] == (0xb0 | ch));
    CHECK(got[0].bytes[1] == 74);
    CHECK(got[0].bytes[2] == 127);
    CHECK(got[1].bytes[0] == (0xd0 | ch)); // the channel's pressure: the note's own
    CHECK(std::abs(got[1].bytes[1] - 64) <= 1);
    CHECK(flagged(got[0]));
    CHECK(flagged(got[1]));

    // A drum pad doesn't slide; nor does a pad no longer held.
    play(player, SurfaceInput::padDown(1, 0, 1.0f));
    drain(engine);
    play(player, SurfaceInput::padSlide(1, 0, 1.0f));
    play(player, SurfaceInput::padSlide(2, 2, 1.0f));
    CHECK(drain(engine).empty());

    // The strip stays on the master channel, for every note.
    play(player, SurfaceInput::strip(1.0f));
    const auto strip = drain(engine);
    REQUIRE(strip.size() == 1);
    CHECK(strip[0].bytes[0] == 0xe0);
    CHECK_FALSE(flagged(strip[0]));

    play(player, SurfaceInput::padUp(0, 0));
    const auto off = drain(engine);
    REQUIRE(off.size() == 1);
    CHECK(off[0].bytes[0] == (0x80 | ch));
    CHECK(flagged(off[0]));
}

TEST_CASE("Engine: input meant for one track plays it alone, armed or not; per-note channels count as channel 1",
          "[control][fastpath][engine][mpe]")
{
    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer{engine, BlockPattern::Fixed64};
    session.newSet(3, 1);
    for (int t = 0; t < 3; ++t)
    {
        auto chain = LiveSet::chainOf(session.set().track(t));
        while (chain.getNumChildren() > 0)
            session.set().removeDevice(chain.getChild(0));
        session.set().insertDevice(chain, -1, TestRecorderDevice::typeId);
    }
    session.set().setTrackArm(0, true, false);
    session.set().track(0).setProperty(ids::midiChannel, 1, nullptr); // channel 1 only
    session.set().setTrackMonitoring(2, Monitoring::Off);
    session.flush();
    renderer.renderFor(512);
    renderer.attach(session);
    const auto id = [&](int t) { return session.set().trackId(t); };

    const auto push = [&](std::uint8_t statusByte, int note, std::uint32_t target, std::uint8_t flags) {
        const std::uint8_t bytes[]{statusByte, static_cast<std::uint8_t>(note), 100};
        engine.midiInput().push(bytes, 3, 5, -1.0, target, flags);
    };
    push(0x90, 40, 0, 0);                                            // the armed track
    push(0x90, 41, static_cast<std::uint32_t>(id(1)), 0);            // track 1 alone, unarmed
    push(0x94, 42, static_cast<std::uint32_t>(id(1)), 0);            // whatever its channel
    push(0x94, 43, 0, 0);                                            // channel 5: not track 0's
    push(0x94, 44, 0, TimedMidi::perNote);                           // per note: channel 1's
    push(0x90, 45, static_cast<std::uint32_t>(id(2)), 0);            // monitoring Off: not heard
    renderer.renderFor(128);

    const auto notes = [&](int t) {
        std::vector<int> out;
        for (const auto& e : renderer.noteOns(id(t)))
            out.push_back(e.data1);
        return out;
    };
    CHECK(notes(0) == std::vector<int>{40, 44});
    CHECK(notes(1) == std::vector<int>{41, 42});
    CHECK(notes(2).empty());
    bool flagKept = false;
    for (const auto& e : renderer.noteOns(id(0)))
        flagKept = flagKept || (e.data1 == 44 && (e.flags & TimedMidi::perNote) != 0);
    CHECK(flagKept); // the instrument sees it per note
}

namespace
{
/** Push, Virtual Push (8x12) and a Launchpad on one set, with the window's selection. */
struct ThreeSurfaces
{
    ThreeSurfaces()
    {
        session.newSet(20, 12);
        session.flush();
        ui::addSessionCommands(commands, session, {[this] { return selection.get(); }, [this](ui::SessionSelection s) { selection.set(s); }}, {});
        const SurfaceContext context{session, commands, selection, feed};
        push = std::make_unique<SurfaceController>(context, pushSurface);
        virtualPush = std::make_unique<SurfaceController>(context, virtualSurface);
        launchpad = std::make_unique<SurfaceController>(context, launchpadSurface);
        refresh();
    }
    void refresh()
    {
        for (auto* c : {push.get(), virtualPush.get(), launchpad.get()})
            c->refresh();
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    ui::EngineStateFeed feed{session, engine};
    ui::CommandRegistry commands{&session.undoManager()};
    ui::SessionSelectionModel selection;
    FakeSurface pushSurface{presets::push2()};
    FakeSurface virtualSurface{presets::virtualPush(8, 12)};
    FakeSurface launchpadSurface{presets::launchpadProMk3()};
    std::unique_ptr<SurfaceController> push, virtualPush, launchpad;
};
} // namespace

TEST_CASE("Several surfaces: those following the window's selection move it together; one with its own keeps to itself",
          "[control][surfaces]")
{
    ThreeSurfaces f;
    // Push's arrows move the selection; Virtual Push's box and the Launchpad's follow it.
    for (int i = 0; i < 13; ++i)
        f.pushSurface.click(Role::navRight);
    CHECK(f.selection.get().track == 13);
    CHECK(f.push->sessionMode().firstColumn() == 6);        // 8 across: 13 is its last
    CHECK(f.virtualPush->sessionMode().firstColumn() == 2); // 12 across
    CHECK(f.launchpad->sessionMode().firstColumn() == 6);
    // The Launchpad's track button selects for all of them.
    f.launchpadSurface.click(Role::trackSelect, 0);
    CHECK(f.selection.get().track == 6);
    CHECK(f.virtualPush->selection().get().track == 6);
    // The LEDs agree: track 6's button lit the same way on each that shows it (Virtual Push's
    // box starts at 2).
    f.refresh();
    CHECK_FALSE(f.launchpadSurface.button(Role::trackSelect, 0).isOff());
    CHECK(f.virtualSurface.button(Role::trackSelect, 4) == f.launchpadSurface.button(Role::trackSelect, 0));

    SECTION("The Launchpad with its own selection: the others don't move it, it doesn't move them")
    {
        f.launchpad->setFollowsSelection(false);
        CHECK(f.launchpad->selection().get() == f.selection.get()); // from where it was
        f.pushSurface.click(Role::navLeft);
        CHECK(f.selection.get().track == 5);
        CHECK(f.launchpad->selection().get().track == 6);
        CHECK(f.launchpad->sessionMode().firstColumn() == 6);
        f.launchpadSurface.click(Role::trackSelect, 3);
        CHECK(f.launchpad->selection().get().track == 9);
        CHECK(f.selection.get().track == 5);
        CHECK(f.virtualPush->selection().get().track == 5);
        // Following again: back on the window's.
        f.launchpad->setFollowsSelection(true);
        CHECK(f.launchpad->selection().get().track == 5);
        CHECK(&f.launchpad->selection() == &f.selection);
    }
}

TEST_CASE("Several surfaces: one with its own selection plays its own track; the following ones play the armed track",
          "[control][surfaces][fastpath]")
{
    ThreeSurfaces f;
    f.selection.set({Kind::track, 2, 0});
    f.launchpad->setFollowsSelection(false);
    f.selection.set({Kind::track, 4, 0});
    for (auto* c : {f.push.get(), f.launchpad.get()})
        c->setMode(ModeKind::note);
    f.refresh();
    CHECK(f.push->padMap().target == 0); // the armed tracks (auto-arm: the window's selected one)
    CHECK(f.launchpad->padMap().target == static_cast<std::uint32_t>(f.session.set().trackId(2)));
    CHECK(f.launchpad->noteMode().track() == 2);
    CHECK(f.push->noteMode().track() == 4);

    drain(f.engine);
    f.launchpadSurface.press(7, 0);
    f.pushSurface.press(7, 0);
    const auto got = drain(f.engine);
    std::vector<std::uint32_t> targets;
    for (const auto& e : got)
        if (status(e) == 0x90)
            targets.push_back(e.target);
    CHECK(targets == std::vector<std::uint32_t>{static_cast<std::uint32_t>(f.session.set().trackId(2)), 0u});

    // Its own track changes: its notes follow.
    f.launchpad->selection().set({Kind::track, 3, 0});
    f.refresh();
    CHECK(f.launchpad->padMap().target == static_cast<std::uint32_t>(f.session.set().trackId(3)));
    f.launchpadSurface.release(7, 0);
    f.pushSurface.release(7, 0);
}

TEST_CASE("Several surfaces: New on a surface with its own selection makes the clip in its own slot", "[control][surfaces]")
{
    ThreeSurfaces f;
    f.selection.set({Kind::slot, 1, 1});
    f.virtualPush->setFollowsSelection(false);
    f.selection.set({Kind::slot, 3, 3});
    f.virtualSurface.click(Role::newClip);
    CHECK(f.session.set().clip(1, 1).isValid());
    CHECK_FALSE(f.session.set().clip(3, 3).isValid());
    f.pushSurface.click(Role::newClip);
    CHECK(f.session.set().clip(3, 3).isValid());
}

TEST_CASE("Surface response: from an input or a change to the lights handed to the driver, timed", "[control][surfaces][response]")
{
    ThreeSurfaces f;
    f.push->responseTimes().reset();
    f.pushSurface.down(Role::shift); // a held button: a refresh soon
    f.push->refresh();               // (the message that follows)
    f.pushSurface.up(Role::shift);
    f.push->refresh();
    f.session.set().setTempo(133.0); // the set: likewise
    f.push->refresh();
    f.push->refresh(); // nothing asked: nothing timed
    const auto s = f.push->responseTimes().summary();
    CHECK(s.count == 3);
    CHECK(s.maxMs < 1000.0 / 60.0);
}
