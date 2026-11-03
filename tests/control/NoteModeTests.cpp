// Note mode (docs/06-m5-controllers.md, 5b; "Note mode: layouts"): the layout formulas and presets,
// the colours for Colundi (anchors, highlights, what can't play) and 12-TET (root, scale), duplicate
// pads lighting together, the Scale/Tuning menu, octave moves, every device size, and notes reaching
// the armed track through the fast path.

#include "support/FakeSurface.h"
#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"
#include "control/modes/NoteMode.h"
#include "control/surface/Presets.h"
#include "control/surface/SurfaceController.h"
#include "tuning/Tet12.h"
#include "ui/commands/SessionCommands.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace lattice;
using namespace lattice::control;
using namespace lattice::test;
using LayoutKind = NoteLayout::Kind;

namespace
{
constexpr std::uint32_t trackRgb = 0x2060e0;

struct Fixture
{
    explicit Fixture(SurfaceCapabilities caps = presets::virtualPush(8, 8), const char* tuning = TuningRegistry::colundiId)
        : surface(std::move(caps))
    {
        session.newSet(3, 2);
        for (int t = 0; t < 3; ++t)
        {
            auto chain = LiveSet::chainOf(session.set().track(t));
            while (chain.getNumChildren() > 0)
                session.set().removeDevice(chain.getChild(0));
            session.set().insertDevice(chain, -1, TestRecorderDevice::typeId);
            session.set().setColour(session.set().track(t), 0xff000000 | trackRgb);
        }
        session.set().setTrackTuning(0, tuning);
        session.flush();
        ui::addSessionCommands(commands, session, {[this] { return selection.get(); }, [this](ui::SessionSelection s) { selection.set(s); }}, {});
        controller = std::make_unique<SurfaceController>(SurfaceContext{session, commands, selection, feed}, surface);
        selection.set({ui::SessionSelection::Kind::track, 0, 0});
        controller->setMode(ModeKind::note);
        controller->refresh();
    }
    NoteMode& mode() { return controller->noteMode(); }
    void render(std::int64_t samples = 64)
    {
        session.flush();
        renderer.renderFor(samples);
        session.processEngineEvents();
        now += 1.0 / 60.0;
        feed.tick(now);
    }
    /** The first pad (from the bottom left) whose note passes the test, or (-1, -1). */
    template <typename Pred>
    std::pair<int, int> findPad(Pred pred)
    {
        const auto& c = surface.caps;
        for (int r = c.rows - 1; r >= 0; --r)
            for (int col = 0; col < c.columns; ++col)
                if (const int n = mode().noteAtPad(r, col); n >= 0 && pred(n, r, col))
                    return {r, col};
        return {-1, -1};
    }

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

TEST_CASE("Note layouts: the table formula, its presets, In Key and Chromatic", "[control][note]")
{
    NoteLayout table;
    table.origin = 30;
    table.rowOffset = 5;
    // index = origin + column x step + row x offset, rows counted from the bottom.
    CHECK(table.noteAt(7, 0, 8) == 30);
    CHECK(table.noteAt(7, 3, 8) == 33);
    CHECK(table.noteAt(6, 0, 8) == 35);
    CHECK(table.noteAt(0, 7, 8) == 30 + 7 + 7 * 5);
    table.columnStep = 2;
    CHECK(table.noteAt(7, 3, 8) == 36);
    table.columnStep = 1;

    // Sequential: no entry twice; 64 of the 128 on an 8x8.
    const auto presets = NoteLayout::rowPresets(LayoutKind::table, 8);
    REQUIRE(presets.size() == 4);
    CHECK(presets[0] == std::pair<juce::String, int>{"Sequential", 8});
    CHECK(presets[1] == std::pair<juce::String, int>{"Overlap 3", 5});
    CHECK(presets[3] == std::pair<juce::String, int>{"Overlap 7", 1});
    auto sequential = table;
    sequential.origin = 0;
    sequential.rowOffset = presets[0].second;
    std::set<int> seen;
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c)
            seen.insert(sequential.noteAt(r, c, 8));
    CHECK(seen.size() == 64);
    CHECK(NoteLayout::rowPresets(LayoutKind::table, 4).size() == 2); // Overlap 5 and 7 need more columns

    // Outside 0..127: no note.
    auto high = table;
    high.origin = 120;
    CHECK(high.noteAt(7, 7, 8) == 127);
    CHECK(high.noteAt(6, 7, 8) == -1);
    auto low = table;
    low.origin = -3;
    CHECK(low.noteAt(7, 0, 8) == -1);
    CHECK(low.noteAt(7, 3, 8) == 0);

    // In Key, C major from C1: the bottom row walks the scale, each row 3 degrees (a 4th) higher.
    const auto tet = Tet12::make(440.0);
    const auto inKey = NoteLayout::defaultFor(*tet);
    CHECK(inKey.kind == LayoutKind::inKey);
    const std::vector<int> bottom{36, 38, 40, 41, 43, 45, 47, 48};
    for (int c = 0; c < 8; ++c)
        CHECK(inKey.noteAt(7, c, 8) == bottom[static_cast<std::size_t>(c)]);
    CHECK(inKey.noteAt(6, 0, 8) == 41); // F
    CHECK(inKey.isRoot(48));
    CHECK(inKey.inScale(40));
    CHECK_FALSE(inKey.inScale(37));
    const auto inD = inKey.withRoot(2);
    CHECK(inD.noteAt(7, 0, 8) == 38);
    CHECK(inD.noteAt(7, 2, 8) == 42); // F#: D major's third

    // Chromatic: semitones, rows a 4th apart.
    const auto chromatic = inKey.withKind(LayoutKind::chromatic, *tet);
    CHECK(chromatic.noteAt(7, 1, 8) == 37);
    CHECK(chromatic.noteAt(6, 0, 8) == 41);

    // A table tuning has only the table layout.
    const TuningRegistry registry;
    const auto colundi = registry.findById(TuningRegistry::colundiId);
    REQUIRE(colundi != nullptr);
    CHECK(NoteLayout::kindsFor(*colundi) == std::vector<LayoutKind>{LayoutKind::table});
    CHECK(NoteLayout::defaultFor(*colundi).kind == LayoutKind::table);
    CHECK(NoteLayout::defaultFor(*colundi).origin == colundi->defaultKeyboardBase());
    CHECK(inKey.withKind(LayoutKind::chromatic, *colundi) == inKey); // not offered: unchanged
}

TEST_CASE("Note layouts: octave moves stop at the ends; a layout is saved and read back", "[control][note]")
{
    NoteLayout l;
    l.origin = 0;
    l.rowOffset = 5;
    CHECK(l.octaveDistance(8) == 5);
    l.octaveStep = NoteLayout::OctaveStep::page;
    CHECK(l.octaveDistance(8) == 40);
    CHECK(l.canShift(1, 8, 8));
    // Down from 0: the grid would still reach up to 0 + 7 + 35 - 40 = 2, so it may move once.
    CHECK(l.canShift(-1, 8, 8));
    const auto down = l.shifted(-1, 8, 8);
    CHECK(down.origin == -40);
    CHECK_FALSE(down.canShift(-1, 8, 8));
    CHECK(down.shifted(-1, 8, 8) == down);
    auto top = l;
    top.origin = 120;
    CHECK_FALSE(top.canShift(1, 8, 8));
    // 12-TET layouts move an octave.
    const auto tet = Tet12::make(440.0);
    CHECK(NoteLayout::defaultFor(*tet).octaveDistance(8) == 12);

    NoteLayout custom;
    custom.kind = LayoutKind::inKey;
    custom.origin = 50;
    custom.rowOffset = 2;
    custom.root = 2;
    custom.scale = 3;
    custom.octaveStep = NoteLayout::OctaveStep::page;
    CHECK(NoteLayout::fromString(custom.toString(), {}) == custom);
    // Rubbish keeps the fallback's values.
    CHECK(NoteLayout::fromString("kind=nonsense;row=999;origin=x", custom) == [&] {
        auto c = custom;
        c.rowOffset = NoteLayout::maxRowOffset;
        return c;
    }());
}

TEST_CASE("Note mode, Colundi: anchors in the track's colour, highlights white, the rest dim, what can't play unlit",
          "[control][note]")
{
    Fixture f;
    const auto l = f.mode().layout();
    CHECK(l.kind == LayoutKind::table);
    // Highlight one entry that isn't an anchor.
    const auto toHighlight = f.findPad([&](int n, int, int) {
        const auto row = f.mode().rowFor(n);
        return !row.anchor && !row.unplayable;
    });
    REQUIRE(toHighlight.first >= 0);
    const int highlighted = f.mode().noteAtPad(toHighlight.first, toHighlight.second);
    f.session.set().setHighlight(f.session.set().track(0), {highlighted});
    f.controller->refresh();
    const auto& colours = f.controller->colours();

    const auto anchor = f.findPad([&](int n, int, int) { return f.mode().rowFor(n).anchor; });
    REQUIRE(anchor.first >= 0);
    CHECK(f.surface.pad(anchor.first, anchor.second) == LedState::solid(trackRgb));
    CHECK(f.surface.pad(toHighlight.first, toHighlight.second) == LedState::solid(colours.white));
    const auto plain = f.findPad([&](int n, int, int) {
        const auto row = f.mode().rowFor(n);
        return !row.anchor && !row.highlighted && !row.unplayable;
    });
    REQUIRE(plain.first >= 0);
    CHECK(f.surface.pad(plain.first, plain.second) == LedState::solid(dimmed(colours.white, colours.padDim)));
    // Labels: the entry; the status line: the track, its tuning and the layout.
    CHECK(f.surface.label(7, 0) == juce::String(l.origin));
    CHECK(f.surface.text.status.contains("Colundi"));
    CHECK(f.surface.text.status.contains("1 highlighted"));

    // The pads play what they show, on the fast path.
    const auto& map = f.controller->padMap();
    for (int r = 0; r < 8; ++r)
        for (int c = 0; c < 8; ++c)
        {
            const int n = f.mode().noteAtPad(r, c);
            const auto& pad = map.pads[static_cast<std::size_t>(r * 8 + c)];
            CHECK(pad.kind == (n >= 0 ? PadNoteMap::Pad::Kind::note : PadNoteMap::Pad::Kind::none));
            if (n >= 0)
                CHECK(pad.note == n);
        }

    SECTION("A press lights its pad and every other pad playing the same entry, and shows its frequency")
    {
        // Rows offset by 5 on 8 columns: (bottom row, column 5) plays what (the row above, column 0) does.
        const int n = f.mode().noteAtPad(7, 5);
        REQUIRE(n == f.mode().noteAtPad(6, 0));
        f.surface.press(7, 5);
        f.controller->refresh();
        CHECK(f.surface.pad(7, 5) == LedState::solid(colours.play));
        CHECK(f.surface.pad(6, 0) == LedState::solid(colours.play));
        CHECK(f.surface.text.status.contains("#" + juce::String(n)));
        CHECK(f.surface.text.status.contains("Hz"));
        f.surface.release(7, 5);
        f.controller->refresh();
        CHECK(f.surface.pad(6, 0) != LedState::solid(colours.play));
    }
    SECTION("At the top of the table: entries too high to play are unlit, red while pressed")
    {
        for (int i = 0; i < 40; ++i)
            f.surface.click(Role::octaveUp);
        f.controller->refresh();
        const auto high = f.findPad([&](int n, int, int) { return f.mode().rowFor(n).unplayable; });
        REQUIRE(high.first >= 0);
        CHECK(f.surface.pad(high.first, high.second).isOff());
        f.surface.press(high.first, high.second);
        f.controller->refresh();
        CHECK(f.surface.pad(high.first, high.second) == LedState::solid(colours.record));
        CHECK(f.surface.button(Role::octaveUp).isOff()); // no further
        f.surface.release(high.first, high.second);
    }
}

TEST_CASE("Note mode, 12-TET: In Key lights the root in the track's colour and the scale white; Chromatic dims the rest",
          "[control][note]")
{
    Fixture f(presets::virtualPush(8, 8), Tet12::builtinId);
    const auto& colours = f.controller->colours();
    CHECK(f.mode().layout().kind == LayoutKind::inKey);
    CHECK(f.surface.pad(7, 0) == LedState::solid(trackRgb)); // C
    CHECK(f.surface.pad(7, 1) == LedState::solid(colours.white)); // D
    CHECK(f.surface.label(7, 0) == f.mode().rowFor(36).label);
    CHECK(f.surface.text.status.contains("C Major"));

    f.surface.click(Role::layout);
    f.controller->refresh();
    CHECK(f.mode().layout().kind == LayoutKind::chromatic);
    CHECK(f.surface.pad(7, 0) == LedState::solid(trackRgb));
    CHECK(f.surface.pad(7, 1) == LedState::solid(dimmed(colours.white, colours.padDim))); // C#
    CHECK(f.surface.pad(7, 2) == LedState::solid(colours.white));                          // D

    // The octave buttons move an octave; with Shift, the In Key root moves a semitone.
    f.surface.click(Role::layout);
    f.surface.click(Role::layout); // Table, then In Key again
    f.controller->refresh();
    REQUIRE(f.mode().layout().kind == LayoutKind::inKey);
    f.surface.click(Role::octaveUp);
    CHECK(f.mode().noteAtPad(7, 0) == 48);
    f.surface.down(Role::shift);
    f.surface.click(Role::octaveUp);
    f.surface.up(Role::shift);
    CHECK(f.mode().layout().root == 1);
    CHECK(f.mode().noteAtPad(7, 0) == 49);
}

TEST_CASE("Note mode: the Scale menu sets the tuning, the layout and the rows; its arrows scroll a small grid",
          "[control][note]")
{
    Fixture f;
    f.surface.click(Role::scale);
    f.controller->refresh();
    REQUIRE(f.mode().menuShown());
    CHECK(f.surface.button(Role::scale) == LedState::solid(f.controller->colours().white));
    // Pads go to the menu, not the fast path.
    for (const auto& pad : f.controller->padMap().pads)
        CHECK(pad.kind == PadNoteMap::Pad::Kind::none);
    CHECK(f.surface.text.status.contains("Tuning: Colundi"));

    // The first row holds the tunings: choose 12-TET (one undo step), and the layout follows it.
    const auto settings = f.mode().menuSettings();
    REQUIRE(settings[0].name == "Tuning");
    const auto at = f.mode().menuPads(settings);
    int tet = -1;
    for (std::size_t i = 0; i < at.size(); ++i)
        if (at[i].first == 0 && settings[0].choices[static_cast<std::size_t>(at[i].second)].name ==
                                    f.tunings.findById(Tet12::builtinId)->name)
            tet = static_cast<int>(i);
    REQUIRE(tet >= 0);
    f.surface.tap(tet / 8, tet % 8);
    CHECK(f.session.set().track(0)[ids::tuningId].toString() == Tet12::builtinId);
    CHECK(f.session.undoManager().getUndoDescription().contains("tuning"));
    CHECK(f.mode().layout().kind == LayoutKind::inKey);
    CHECK(f.surface.text.status.contains("Tuning: "));

    // The rows: Sequential.
    const auto now = f.mode().menuSettings();
    const auto pads = f.mode().menuPads(now);
    for (std::size_t i = 0; i < pads.size(); ++i)
        if (pads[i].first >= 0 && now[static_cast<std::size_t>(pads[i].first)].name == "Rows" &&
            now[static_cast<std::size_t>(pads[i].first)].choices[static_cast<std::size_t>(pads[i].second)].name == "Sequential")
        {
            f.surface.tap(static_cast<int>(i) / 8, static_cast<int>(i) % 8);
            break;
        }
    CHECK(f.mode().layout().rowOffset == 8);

    // Closing it plays notes again.
    f.surface.click(Role::scale);
    f.controller->refresh();
    CHECK_FALSE(f.mode().menuShown());
    CHECK(f.controller->padMap().pads[56].kind == PadNoteMap::Pad::Kind::note);

    SECTION("On a 4x4 grid the menu scrolls")
    {
        Fixture small(presets::virtualPush(4, 4), Tet12::builtinId);
        small.surface.click(Role::scale);
        small.controller->refresh();
        CHECK(small.surface.button(Role::navUp).isOff());
        CHECK_FALSE(small.surface.button(Role::navDown).isOff());
        const auto before = small.surface.label(0, 0);
        small.surface.click(Role::navDown);
        small.controller->refresh();
        CHECK(small.surface.label(0, 0) != before);
        CHECK_FALSE(small.surface.button(Role::navUp).isOff());
    }
}

TEST_CASE("Note mode: the layout is kept on the track, not undoable; another tuning starts from its own default",
          "[control][note]")
{
    Fixture f;
    const auto start = f.mode().layout();
    const int undoBefore = f.session.undoManager().getNumActionsInCurrentTransaction();
    f.surface.click(Role::octaveUp);
    CHECK(f.mode().layout().origin == start.origin + start.rowOffset);
    CHECK(f.session.set().track(0)[ids::noteLayout].toString().contains("tuning=builtin.colundi"));
    CHECK(f.session.undoManager().getNumActionsInCurrentTransaction() == undoBefore);
    CHECK_FALSE(f.session.snapshots().isDirty()); // the engine doesn't rebuild for it

    // Left and right choose the track (each its own layout).
    f.surface.click(Role::navRight);
    CHECK(f.mode().track() == 1);
    CHECK(f.mode().layout().kind == LayoutKind::inKey); // track 1 is 12-TET
    f.surface.click(Role::navLeft);
    CHECK(f.mode().layout().origin == start.origin + start.rowOffset);

    // The tuning changes: its default, not the old table position.
    f.session.set().setTrackTuning(0, Tet12::builtinId);
    CHECK(f.mode().layout() == NoteLayout::defaultFor(*f.tunings.findById(Tet12::builtinId)));

    // A group or nothing selected: nothing to play.
    f.selection.showDevicesOf({ui::DeviceOwner::Kind::master, 0});
    f.controller->refresh();
    CHECK(f.mode().track() == -1);
    CHECK(f.surface.text.status.contains("select a MIDI track"));
    for (const auto& pad : f.controller->padMap().pads)
        CHECK(pad.kind == PadNoteMap::Pad::Kind::none);
}

TEST_CASE("Note mode: on every device, every pad plays a note in range or nothing, and the grid can be moved",
          "[control][note]")
{
    for (const auto& caps : presets::allForTests())
    {
        if (caps.numPads() == 0)
            continue;
        INFO(caps.name.toStdString() << " " << caps.rows << "x" << caps.columns);
        Fixture f(caps);
        int playing = 0;
        for (int r = 0; r < caps.rows; ++r)
            for (int c = 0; c < caps.columns; ++c)
            {
                const int n = f.mode().noteAtPad(r, c);
                CHECK(n >= -1);
                CHECK(n <= 127);
                playing += n >= 0 ? 1 : 0;
            }
        CHECK(playing >= std::min(caps.numPads(), 64)); // a 16x16 grid reaches past the table's top
        // Without octave buttons, the arrows or the strip's scroll still move it (the up arrow).
        const auto before = f.mode().layout().origin;
        if (caps.has(Role::octaveUp))
            f.surface.click(Role::octaveUp);
        else if (caps.has(Role::navUp))
            f.surface.click(Role::navUp);
        else
        {
            auto e = f.controller->expression();
            e.strip = Expression::Strip::scroll;
            f.controller->setExpression(e);
            f.surface.send(SurfaceInput::strip(0.0f));
            f.surface.send(SurfaceInput::strip(0.2f));
        }
        CHECK(f.mode().layout().origin > before);
    }
}

TEST_CASE("Note mode: a pad's note reaches the armed track through the fast path", "[control][note][fastpath]")
{
    Fixture f;
    f.session.set().setTrackArm(0, true, false);
    f.render(512);
    f.renderer.attach(f.session);
    const int n = f.mode().noteAtPad(7, 2);
    f.surface.press(7, 2, 1.0f);
    f.render(64);
    f.surface.release(7, 2);
    f.render(64);
    const auto ons = f.renderer.noteOns(f.session.set().trackId(0));
    REQUIRE(ons.size() == 1);
    CHECK(ons[0].data1 == n);
    CHECK(ons[0].data2 == 127);
    CHECK(f.renderer.unbalancedNotes(f.session.set().trackId(0)).empty());
    // A track that isn't armed (and monitors Auto) doesn't hear it.
    CHECK(f.renderer.noteOns(f.session.set().trackId(1)).empty());
}

TEST_CASE("Note mode: the transport buttons follow the engine as in Session mode", "[control][note]")
{
    Fixture f;
    f.render();
    CHECK(f.surface.button(Role::play) != LedState::solid(f.controller->colours().play));
    f.session.play();
    f.render(); // the feed's tick reaches the controller: no refresh by hand
    CHECK(f.surface.button(Role::play) == LedState::solid(f.controller->colours().play));
}
