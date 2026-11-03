// The surface types and Session mode (docs/06-m5-controllers.md, 5a; Tests 1), on FakeSurface with
// every candidate device's capabilities: the pad->slot mapping, launching, scenes, navigation, the
// overview, the hold modifiers, the global buttons, LED colours and animations, what's sent to the
// driver, and graceful degradation on a device with only a grid.

#include "support/FakeSurface.h"
#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"
#include "control/modes/SessionMode.h"
#include "control/surface/Presets.h"
#include "control/surface/SurfaceController.h"
#include "ui/commands/SessionCommands.h"

#include <catch2/catch_test_macros.hpp>

using namespace lattice;
using namespace lattice::control;
using namespace lattice::test;
using Kind = ui::SessionSelection::Kind;
using Anim = LedState::Animation;

namespace
{
struct Fixture
{
    explicit Fixture(SurfaceCapabilities caps = presets::virtualPush(8, 8), int tracks = 4, int scenes = 4) : surface(std::move(caps))
    {
        session.newSet(tracks, scenes);
        session.flush();
        ui::addSessionCommands(commands, session, {[this] { return selection.get(); }, [this](ui::SessionSelection s) { selection.set(s); }}, {});
        controller = std::make_unique<SurfaceController>(SurfaceContext{session, commands, selection, feed}, surface);
    }

    void render(std::int64_t samples = 64)
    {
        session.flush();
        renderer.renderFor(samples);
        session.processEngineEvents();
        now += 1.0 / 60.0;
        feed.tick(now);
        controller->refresh();
    }
    void refresh() { controller->refresh(); }
    juce::ValueTree clip(int track, int scene, juce::uint32 argb = 0xff2060e0)
    {
        auto c = session.set().createClip(track, scene, 4.0);
        session.set().setColour(c, argb);
        return c;
    }
    SessionMode& mode() { return controller->sessionMode(); }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer{engine, BlockPattern::Fixed64};
    ui::EngineStateFeed feed{session, engine};
    ui::CommandRegistry commands{&session.undoManager()};
    ui::SessionSelectionModel selection;
    FakeSurface surface;
    std::unique_ptr<SurfaceController> controller;
    double now = 3.0;
};
} // namespace

TEST_CASE("Surface: role names are stable and round-trip; numbered roles carry their index", "[control][surface]")
{
    for (int r = 0; r < static_cast<int>(Role::count); ++r)
    {
        const auto role = static_cast<Role>(r);
        CHECK(juce::String(roleName(role)).isNotEmpty());
        CHECK(roleFromName(roleName(role)) == role);
    }
    CHECK(juce::String(roleName(Role::modeSession)) == "mode.session");
    CHECK(buttonName({Role::scene, 3}) == "scene[3]");
    CHECK(buttonName({Role::play, 0}) == "play");
    CHECK_FALSE(roleFromName("no.such.role").has_value());
}

TEST_CASE("Surface: LED frames list what changed; animations follow the beat; dimmed lights stay on", "[control][surface]")
{
    const auto caps = presets::push2();
    auto a = LedFrame::blank(caps);
    REQUIRE(a.pads.size() == 64);
    REQUIRE(a.buttons.size() == caps.buttons.size());
    auto b = a;
    CHECK(b.changesFrom(a).empty());
    b.pads[5] = LedState::solid(0xff0000);
    b.buttons[2] = LedState::blink(0x00ff00);
    const auto changes = b.changesFrom(a);
    CHECK(changes.pads == std::vector<int>{5});
    CHECK(changes.buttons == std::vector<int>{2});
    CHECK(b.anyAnimated());
    CHECK(b.changesFrom(LedFrame{}).pads.size() == 64); // nothing shown before: all of it

    CHECK(animationLevel(Anim::solid, 0.37) == 1.0f);
    CHECK(animationLevel(Anim::blink, 0.0) == 1.0f);
    CHECK(animationLevel(Anim::blink, 0.3) == 0.0f);
    CHECK(animationLevel(Anim::blink, 0.5) == 1.0f);
    CHECK(animationLevel(Anim::pulse, 0.0) > 0.99f);
    CHECK(animationLevel(Anim::pulse, 0.5) < 0.4f);
    CHECK(dimmed(0x010101, 0.1f) != 0);
    CHECK(dimmed(0xff8000, 0.5f) == 0x804000);
}

TEST_CASE("Session mode: on every device, pads map to the slots in the box and outside the set stay dark", "[control][session]")
{
    for (const auto& caps : presets::allForTests())
    {
        INFO(caps.name.toStdString() << " " << caps.rows << "x" << caps.columns);
        Fixture f(caps, 20, 20);
        auto& m = f.mode();
        const bool sceneColumn = !caps.has(ButtonId{Role::scene, 0});
        CHECK(m.scenesOnGrid() == sceneColumn);
        CHECK(m.trackColumns() == caps.columns - (sceneColumn ? 1 : 0));
        CHECK(m.sceneRows() == caps.rows);
        for (int r = 0; r < caps.rows; ++r)
            for (int c = 0; c < m.trackColumns(); ++c)
                CHECK(m.slotAt(r, c) == std::pair{c, r});
        if (sceneColumn)
            CHECK_FALSE(m.slotAt(0, caps.columns - 1).has_value());

        // A small set: pads past its last track and scene are off.
        Fixture small(caps, 2, 2);
        small.clip(1, 1);
        small.refresh();
        CHECK_FALSE(small.mode().slotAt(0, 2).has_value());
        CHECK(small.surface.pad(1, 1).rgb == 0x2060e0);
        for (int r = 0; r < caps.rows; ++r)
            for (int c = 2; c < small.mode().trackColumns(); ++c)
                CHECK(small.surface.pad(r, c).isOff());
    }
}

TEST_CASE("Session mode: a pad launches its clip and selects it; LEDs blink while queued, pulse while playing", "[control][session]")
{
    Fixture f;
    f.clip(1, 2, 0xffe08020);
    f.refresh();
    CHECK(f.surface.pad(2, 1) == LedState::solid(0xe08020));

    f.session.play();
    f.render();
    f.surface.press(2, 1);
    CHECK(f.selection.get() == ui::SessionSelection{Kind::slot, 1, 2});
    f.render();
    // Queued for the next bar, then playing.
    CHECK(f.feed.slot(1, 2) == SlotState::QueuedLaunch);
    CHECK(f.surface.pad(2, 1) == LedState::blink(0xe08020));
    f.render(static_cast<std::int64_t>(48000 * 2.1)); // past bar 2 at 120 BPM
    CHECK(f.feed.slot(1, 2) == SlotState::Playing);
    CHECK(f.surface.pad(2, 1) == LedState::pulse(0xe08020));
    f.surface.release(2, 1);
}

TEST_CASE("Session mode: with Shift a pad only selects; Delete and Duplicate held act on the clip, one undo step each", "[control][session]")
{
    Fixture f;
    f.clip(0, 0);
    f.surface.down(Role::shift);
    f.surface.tap(0, 0);
    f.surface.up(Role::shift);
    f.render();
    CHECK(f.selection.get() == ui::SessionSelection{Kind::slot, 0, 0});
    CHECK(f.feed.slot(0, 0) == SlotState::Stopped);

    f.surface.down(Role::duplicate);
    f.surface.tap(0, 0);
    f.surface.up(Role::duplicate);
    CHECK(f.session.set().clip(0, 1).isValid());

    f.surface.down(Role::deleteClip);
    f.surface.tap(0, 0);
    f.surface.up(Role::deleteClip);
    CHECK_FALSE(f.session.set().clip(0, 0).isValid());
    CHECK(f.session.set().clip(0, 1).isValid());
    f.session.undoManager().undo();
    CHECK(f.session.set().clip(0, 0).isValid());
}

TEST_CASE("Session mode: an empty slot records on an armed track; on another track the first press only selects", "[control][session]")
{
    Fixture f;
    REQUIRE(f.session.isArmed(0)); // a new set arms its first track
    REQUIRE_FALSE(f.session.isArmed(1));
    f.refresh();
    CHECK(f.surface.pad(0, 0).rgb != 0); // armed and empty: dim red
    CHECK(f.surface.pad(0, 1).isOff());

    f.surface.tap(0, 1);
    f.render();
    CHECK(f.selection.get().track == 1);
    CHECK(f.feed.slot(1, 0) == SlotState::Empty);

    f.surface.tap(1, 0);
    f.render();
    const auto state = f.feed.slot(0, 1);
    CHECK((state == SlotState::QueuedRecord || state == SlotState::Recording));
    CHECK(f.surface.pad(1, 0).rgb == f.controller->colours().record);
}

TEST_CASE("Session mode: scene buttons launch scenes; a device without them uses the grid's last column", "[control][session]")
{
    SECTION("Scene buttons")
    {
        Fixture f;
        f.clip(0, 1);
        f.clip(2, 1);
        f.session.play();
        f.render();
        f.surface.click(Role::scene, 1);
        f.render();
        CHECK(f.feed.slot(0, 1) == SlotState::QueuedLaunch);
        CHECK(f.feed.slot(2, 1) == SlotState::QueuedLaunch);
        CHECK(f.surface.button(Role::scene, 1) == LedState::blink(f.controller->colours().play));
        CHECK(f.surface.button(Role::scene, 0).rgb != 0); // a scene inside the set
    }
    SECTION("Grid only")
    {
        Fixture f(presets::gridOnly(4, 4));
        f.clip(0, 2);
        f.session.play();
        f.render();
        f.surface.tap(2, 3);
        f.render();
        CHECK(f.feed.slot(0, 2) == SlotState::QueuedLaunch);
        CHECK(f.surface.pad(2, 3) == LedState::blink(f.controller->colours().play));
    }
}

TEST_CASE("Session mode: the arrows move the selection and the box follows; Shift+arrow moves a page", "[control][session]")
{
    Fixture f(presets::virtualPush(4, 4), 12, 12);
    auto& m = f.mode();
    f.selection.set({Kind::slot, 0, 0});
    for (int i = 0; i < 5; ++i)
        f.surface.click(Role::navDown);
    CHECK(f.selection.get().scene == 5);
    CHECK(m.firstScene() == 2); // scenes 2-5 in view
    f.surface.click(Role::navRight);
    CHECK(f.selection.get() == ui::SessionSelection{Kind::slot, 1, 5});

    f.surface.down(Role::shift);
    f.surface.click(Role::navRight);
    f.surface.up(Role::shift);
    CHECK(m.firstColumn() == 4);
    CHECK(f.selection.get().track == 5);

    // The selection made on screen moves the box too.
    f.selection.set({Kind::slot, 11, 11});
    CHECK(m.firstColumn() == 8);
    CHECK(m.firstScene() == 8);
    f.refresh();
    CHECK(f.surface.button(Role::navDown).isOff());
    CHECK_FALSE(f.surface.button(Role::navUp).isOff());

    SECTION("Not following: the arrows move the box only")
    {
        f.controller->setFollowsSelection(false);
        f.surface.click(Role::navUp);
        CHECK(m.firstScene() == 7);
        CHECK(f.selection.get().scene == 11);
    }
}

TEST_CASE("Session mode: a folded group hides its tracks from the box", "[control][session]")
{
    Fixture f(presets::virtualPush(8, 8), 5, 4);
    auto& set = f.session.set();
    const int group = set.groupTracks(1, 2);
    set.setGroupFolded(group, true);
    const auto columns = f.mode().columnsOfSet();
    CHECK(columns == std::vector<int>{0, group, set.blockEnd(group), set.blockEnd(group) + 1});
    CHECK(f.mode().slotAt(0, 1) == std::pair{group, 0});
}

TEST_CASE("Session mode: holding Session shows the overview; a pad moves the box to its page", "[control][session]")
{
    Fixture f(presets::virtualPush(4, 4), 10, 9);
    f.clip(9, 8);
    f.surface.down(Role::modeSession);
    CHECK(f.mode().overviewShown());
    f.refresh();
    // Pages: 3 across (10 tracks / 4), 3 down (9 scenes / 4).
    CHECK(f.surface.pad(0, 0) == LedState::solid(f.controller->colours().white)); // the box is here
    CHECK_FALSE(f.surface.pad(2, 2).isOff());                                     // a page with a clip
    CHECK(f.surface.pad(0, 3).isOff());                                           // past the set
    CHECK(f.surface.pad(3, 0).isOff());
    f.surface.tap(2, 2);
    f.surface.up(Role::modeSession);
    CHECK(f.mode().firstColumn() == 6); // clamped: the last page shows the last 4 tracks
    CHECK(f.mode().firstScene() == 5);
    CHECK_FALSE(f.mode().overviewShown());
}

TEST_CASE("Session mode: Stop Clip, Mute and Solo held change what the track buttons do", "[control][session]")
{
    Fixture f;
    f.clip(1, 0);
    f.session.play();
    f.render();
    f.surface.tap(0, 1);
    f.render(static_cast<std::int64_t>(48000 * 2.1));
    REQUIRE(f.feed.slot(1, 0) == SlotState::Playing);

    f.surface.down(Role::stopClip);
    f.refresh();
    CHECK(f.surface.button(Role::trackSelect, 1) == LedState::solid(f.controller->colours().record));
    f.surface.click(Role::trackSelect, 1);
    f.surface.up(Role::stopClip);
    f.render();
    CHECK(f.feed.slot(1, 0) == SlotState::QueuedStop);

    f.surface.down(Role::mute);
    f.surface.click(Role::trackSelect, 2);
    f.surface.up(Role::mute);
    CHECK(static_cast<bool>(f.session.set().track(2)[ids::mute]));
    f.surface.down(Role::solo);
    f.surface.click(Role::trackSelect, 3);
    f.surface.up(Role::solo);
    CHECK(static_cast<bool>(f.session.set().track(3)[ids::solo]));

    f.surface.click(Role::trackSelect, 2);
    CHECK(f.selection.get() == ui::SessionSelection{Kind::track, 2, 0});
    f.refresh();
    CHECK(f.surface.button(Role::trackSelect, 2) == LedState::solid(f.controller->colours().white));
}

TEST_CASE("Session mode: without track buttons, a held modifier turns the bottom row into track pads", "[control][session]")
{
    auto caps = presets::gridOnly(8, 8);
    caps.buttons.push_back({ButtonId{Role::mute, 0}, LedKind::single});
    Fixture f(caps);
    CHECK_FALSE(f.mode().trackRowShown());
    f.surface.down(Role::mute);
    CHECK(f.mode().trackRowShown());
    CHECK(f.mode().sceneRows() == 7);
    f.refresh();
    CHECK(f.surface.pad(7, 1).rgb == f.controller->colours().mute); // on
    f.surface.tap(7, 1);
    f.surface.up(Role::mute);
    CHECK(static_cast<bool>(f.session.set().track(1)[ids::mute]));
    CHECK(f.mode().sceneRows() == 8);
}

TEST_CASE("Surface controller: the global buttons run the transport, undo and the metronome, and light up", "[control][session]")
{
    Fixture f(presets::push2());
    f.refresh();
    const auto& colours = f.controller->colours();
    CHECK(f.surface.button(Role::play).rgb == dimmed(colours.white, colours.dim));
    f.surface.click(Role::play);
    f.render();
    CHECK(f.feed.transport().playing);
    CHECK(f.surface.button(Role::play) == LedState::solid(colours.play));
    f.surface.click(Role::play);
    f.render();
    CHECK_FALSE(f.feed.transport().playing);

    const bool metronome = f.session.set().metronomeOn();
    f.surface.click(Role::metronome);
    CHECK(f.session.set().metronomeOn() != metronome);

    f.clip(0, 0);
    f.surface.down(Role::deleteClip);
    f.surface.tap(0, 0);
    f.surface.up(Role::deleteClip);
    REQUIRE_FALSE(f.session.set().clip(0, 0).isValid());
    f.surface.click(Role::undo);
    CHECK(f.session.set().clip(0, 0).isValid());
    f.surface.down(Role::shift);
    f.surface.click(Role::undo);
    f.surface.up(Role::shift);
    CHECK_FALSE(f.session.set().clip(0, 0).isValid());

    f.refresh();
    CHECK(f.surface.button(Role::modeSession) == LedState::solid(colours.white));
    CHECK(f.surface.button(Role::modeDevice).isOff()); // not built yet
}

TEST_CASE("Surface controller: only what changed goes to the driver, and nothing when nothing did", "[control][surface]")
{
    Fixture f;
    f.refresh();
    const auto frames = f.surface.frames;
    f.refresh();
    f.render();
    CHECK(f.surface.frames == frames);

    f.clip(3, 3);
    f.refresh();
    CHECK(f.surface.frames == frames + 1);
    CHECK(f.surface.lastChanges.pads == std::vector<int>{3 * 8 + 3});
}
