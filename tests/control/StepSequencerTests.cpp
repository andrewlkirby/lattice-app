// The step sequencers (docs/06-m5-controllers.md, 5d; Tests 1, "Drum"): a clip's notes as steps
// (the step a note shows on, tied and muted steps, adding and removing), the held step's
// velocity, length and nudge as undoable bindings, the zones on every grid size, the Drum layout
// (its pads playing and choosing the pad, the steps, the loop selector, moving the pads), the
// melodic sequencer in the track's tuning, held steps taking the encoders, and the playhead.

#include "support/FakeSurface.h"
#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"
#include "control/modes/NoteMode.h"
#include "control/surface/Presets.h"
#include "control/surface/SurfaceController.h"
#include "devices/racks/Rack.h"
#include "devices/racks/RackEditing.h"
#include "ui/clip/Steps.h"
#include "ui/commands/SessionCommands.h"
#include "ui/mapping/Targets.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace lattice;
using namespace lattice::control;
using namespace lattice::test;
using Catch::Matchers::WithinAbs;
using Kind = ui::SessionSelection::Kind;
namespace steps = ui::steps;

namespace
{
constexpr std::uint32_t trackRgb = 0x2060e0;

struct Fixture
{
    /** Track 0 has a Drum Rack (pads on 36 and 38), track 1 a plain instrument in Colundi. */
    explicit Fixture(SurfaceCapabilities caps = presets::virtualPush(8, 8)) : surface(std::move(caps))
    {
        session.newSet(2, 4);
        auto& set = session.set();
        for (int t = 0; t < 2; ++t)
        {
            auto chain = LiveSet::chainOf(set.track(t));
            while (chain.getNumChildren() > 0)
                set.removeDevice(chain.getChild(0));
            set.setColour(set.track(t), 0xff000000 | trackRgb);
        }
        rack = racks::insertRack(set, LiveSet::chainOf(set.track(0)), -1, RackDevice::drumRackId);
        racks::addDrumPad(set, rack, 36);
        racks::addDrumPad(set, rack, 38);
        set.insertDevice(LiveSet::chainOf(set.track(1)), -1, TestRecorderDevice::typeId);
        set.setTrackTuning(1, TuningRegistry::colundiId);
        session.flush();
        ui::addSessionCommands(commands, session, {[this] { return selection.get(); }, [this](ui::SessionSelection s) { selection.set(s); }}, {});
        controller = std::make_unique<SurfaceController>(SurfaceContext{session, commands, selection, feed, &specs}, surface);
        selection.set({Kind::slot, 0, 0});
        controller->setMode(ModeKind::note);
        mode().sequencer().clock = [this] { return clock; };
        refresh();
    }
    ~Fixture() { controller.reset(); }

    NoteMode& mode() { return controller->noteMode(); }
    void refresh() { controller->refresh(); }
    juce::ValueTree clip(int t = 0, int s = 0) { return session.set().clip(t, s); }
    std::vector<NoteData> notes(int t = 0, int s = 0) { return LiveSet::notesOf(clip(t, s)); }
    void tapPad(int row, int column)
    {
        surface.press(row, column);
        surface.release(row, column);
        refresh();
    }
    void render(std::int64_t samples)
    {
        session.flush();
        renderer.renderFor(samples);
        session.processEngineEvents();
        now += 1.0 / 60.0;
        feed.tick(now);
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer{engine, BlockPattern::Fixed64};
    DeviceSpecs specs{testRegistry()};
    ui::EngineStateFeed feed{session, engine};
    ui::CommandRegistry commands{&session.undoManager()};
    ui::SessionSelectionModel selection;
    FakeSurface surface;
    std::unique_ptr<SurfaceController> controller;
    juce::ValueTree rack;
    juce::uint32 clock = 1000;
    double now = 3.0;
};
} // namespace

TEST_CASE("Steps: the step a note shows on, tied and muted steps, adding and removing", "[control][steps]")
{
    const double q = 0.25;
    CHECK(steps::stepOf(0.0, q) == 0);
    CHECK(steps::stepOf(0.12, q) == 0);  // nudged later, still its step
    CHECK(steps::stepOf(0.13, q) == 1);  // past half a step: the next one's
    CHECK(steps::stepOf(0.38, q) == 2);
    CHECK(steps::stepOf(-0.1, q) == 0);

    const std::vector<NoteData> notes{{36, 0.0, 0.75, 100, false}, {36, 0.5, 0.25, 90, true}, {38, 0.25, 0.25, 80, false},
                                      {36, 1.02, 0.2, 70, false}};
    using S = steps::State;
    CHECK(steps::row(notes, 36, q, 0, 6) == std::vector<S>{S::on, S::tied, S::muted, S::off, S::on, S::off});
    CHECK(steps::row(notes, 36, q, 2, 3) == std::vector<S>{S::muted, S::off, S::on}); // from step 2
    CHECK(steps::row(notes, 38, q, 0, 3) == std::vector<S>{S::off, S::on, S::off});
    CHECK(steps::notesIn(notes, {36, 4}, q) == steps::notesIn(notes, std::vector<steps::Step>{{36, 4}}, q));
    CHECK(steps::notesIn(notes, {36, 4}, q) == ui::notes::Indices{3});

    // A step with notes loses them; an empty one gets a note a step long.
    const auto off = steps::toggle(notes, {36, 0}, q);
    CHECK(off.removed == ui::notes::Indices{0});
    CHECK(off.added.empty());
    const auto on = steps::toggle(notes, {40, 3}, q);
    CHECK(on.removed.empty());
    REQUIRE(on.added.size() == 1);
    CHECK(on.added[0] == NoteData{40, 0.75, q, steps::defaultVelocity, false});
}

TEST_CASE("Steps: a held step's velocity, length and nudge are undoable, and addresses find them", "[control][steps]")
{
    Fixture f;
    auto& set = f.session.set();
    auto& undo = f.session.undoManager();
    auto c = set.createClip(0, 1, 4.0);
    set.addNote(c, {36, 0.5, 0.25, 100, false});
    set.addNote(c, {36, 0.5, 0.25, 60, false}); // two on the same step: they're edited together
    set.addNote(c, {38, 0.5, 0.25, 50, false});
    undo.clearUndoHistory();
    const std::vector<steps::Step> held{{36, 2}};

    auto velocity = steps::binding(set, &undo, c, held, 0.25, steps::Field::velocity);
    REQUIRE(velocity != nullptr);
    CHECK(velocity->value() == 100.0f); // the first note's
    undo.beginNewTransaction();
    velocity->setValue(30.0f);
    for (const auto& n : LiveSet::notesOf(c))
        CHECK(n.velocity == (n.pitch == 36 ? 30 : 50));
    CHECK(undo.getNumActionsInCurrentTransaction() > 0);

    auto length = ui::bindingForTarget(set, f.specs, &undo, steps::target(c, held, 0.25, steps::Field::length));
    REQUIRE(length != nullptr);
    CHECK_THAT(length->value(), WithinAbs(0.25, 1.0e-6));
    CHECK(length->text() == "1.00 steps");
    length->setValue(0.75f);
    CHECK_THAT(LiveSet::notesOf(c)[0].length, WithinAbs(0.75, 1.0e-6));

    auto nudge = ui::bindingForTarget(set, f.specs, &undo, steps::target(c, held, 0.25, steps::Field::nudge));
    REQUIRE(nudge != nullptr);
    nudge->setValue(0.1f);
    CHECK_THAT(LiveSet::notesOf(c)[0].start, WithinAbs(0.6, 1.0e-6));
    CHECK(nudge->text() == "40 %");
    nudge->setValue(1.0f); // no further than just under half a step: it stays on its step
    CHECK(steps::stepOf(LiveSet::notesOf(c)[0].start, 0.25) == 2);
    CHECK(nudge->value() > 0.1f);

    // Each change one undo step; undoing them all puts the notes back.
    while (undo.canUndo())
        undo.undo();
    const auto back = LiveSet::notesOf(c);
    CHECK(back[0] == NoteData{36, 0.5, 0.25, 100, false});
    CHECK(back[1] == NoteData{36, 0.5, 0.25, 60, false});

    // Nothing on the steps: no binding.
    CHECK(steps::binding(set, &undo, c, {{36, 7}}, 0.25, steps::Field::velocity) == nullptr);
    CHECK(ui::bindingForTarget(set, f.specs, &undo, "notes:999/0.25/36:2/velocity") == nullptr);
}

TEST_CASE("Sequencer zones fit every grid without overlapping", "[control][steps]")
{
    const auto drum8 = SequencerZones::drum(8, 8, false);
    CHECK(drum8.steps == GridArea{0, 0, 4, 8}); // 32 steps
    CHECK(drum8.pads == GridArea{4, 0, 4, 4});  // 16 pads
    CHECK(drum8.loop == GridArea{4, 4, 4, 4});  // 16 pages
    CHECK(SequencerZones::drum(8, 8, true).pads == GridArea{0, 0, 8, 8}); // 64 pads
    CHECK(SequencerZones::drum(16, 16, false).steps.count() == 128);
    CHECK(SequencerZones::drum(16, 16, false).pads.count() == 64);
    CHECK(SequencerZones::drum(8, 12, false).steps.count() == 48);
    // Too small for all three: the pads, or the steps over a row of pages.
    CHECK(SequencerZones::drum(4, 4, true).pads == GridArea{0, 0, 4, 4});
    const auto drum4 = SequencerZones::drum(4, 4, false);
    CHECK(drum4.pads.empty());
    CHECK(drum4.steps == GridArea{0, 0, 3, 4});
    CHECK(drum4.loop == GridArea{3, 0, 1, 4});
    CHECK(SequencerZones::melodic(8, 8).steps == GridArea{0, 0, 7, 8});
    CHECK(SequencerZones::melodic(8, 8).loop == GridArea{7, 0, 1, 8});
    CHECK(SequencerZones::melodic(16, 16).loop.rows == 2);

    for (const auto& caps : presets::allForTests())
        for (const int kind : {0, 1, 2})
        {
            if (caps.rows <= 0)
                continue;
            INFO(caps.name << " " << kind);
            const auto z = kind == 2 ? SequencerZones::melodic(caps.rows, caps.columns) : SequencerZones::drum(caps.rows, caps.columns, kind == 1);
            std::vector<int> used(static_cast<std::size_t>(caps.numPads()), 0);
            for (const auto* a : {&z.steps, &z.loop, &z.pads})
            {
                CHECK(a->row >= 0);
                CHECK(a->column >= 0);
                CHECK(a->row + a->rows <= caps.rows);
                CHECK(a->column + a->columns <= caps.columns);
                for (int r = a->row; r < a->row + a->rows; ++r)
                    for (int c = a->column; c < a->column + a->columns; ++c)
                        ++used[static_cast<std::size_t>(r * caps.columns + c)];
            }
            CHECK(std::all_of(used.begin(), used.end(), [](int n) { return n == 1; })); // the whole grid, once
            CHECK((kind == 1 ? !z.pads.empty() : !z.steps.empty()));
        }
}

TEST_CASE("Drum layout: the pads play and choose the steps' pad; steps add and remove notes, one undo step each", "[control][steps]")
{
    Fixture f;
    auto& undo = f.session.undoManager();
    REQUIRE(f.mode().pads() == NoteMode::Pads::drum);
    CHECK(f.mode().drumStart() == 36);
    CHECK(f.mode().drumNote() == 36); // the first pad with something on it
    CHECK(f.mode().drumNoteAtPad(7, 0) == 36);
    CHECK(f.mode().drumNoteAtPad(4, 3) == 36 + 15);
    CHECK(f.mode().drumNoteAtPad(0, 0) == -1); // a step

    // The pads are on the fast path; the steps and the loop selector aren't.
    const auto& map = f.controller->padMap();
    CHECK(map.pads[7 * 8 + 0].kind == PadNoteMap::Pad::Kind::note);
    CHECK(map.pads[7 * 8 + 2].note == 38);
    CHECK(map.pads[0].kind == PadNoteMap::Pad::Kind::none);
    CHECK(map.pads[4 * 8 + 4].kind == PadNoteMap::Pad::Kind::none);

    // Pads with something on them in their colour, the steps' pad white.
    CHECK(f.surface.pad(7, 0) == LedState::solid(f.controller->colours().white));
    CHECK(f.surface.pad(7, 2) == LedState::solid(trackRgb));
    f.tapPad(7, 2);
    CHECK(f.mode().drumNote() == 38);
    CHECK(f.surface.pad(7, 2) == LedState::solid(f.controller->colours().white));

    // The first step makes a clip in the selected slot, a page (32 sixteenths) long.
    undo.clearUndoHistory();
    REQUIRE(!f.clip().isValid());
    f.tapPad(0, 4);
    REQUIRE(f.clip().isValid());
    CHECK_THAT(static_cast<double>(f.clip()[ids::loopEnd]), WithinAbs(8.0, 1.0e-9));
    REQUIRE(f.notes().size() == 1);
    CHECK(f.notes()[0] == NoteData{38, 1.0, 0.25, steps::defaultVelocity, false});
    CHECK(f.surface.pad(0, 4) == LedState::solid(trackRgb));
    CHECK(undo.getUndoDescriptions().size() == 1); // the clip and its note: one step
    // The second row is steps 8 to 15.
    f.tapPad(1, 0);
    CHECK(f.notes().back().start == 2.0);

    // Tapped again, a step loses its notes; held a while (or edited), it keeps them.
    f.tapPad(1, 0);
    CHECK(f.notes().size() == 1);
    f.surface.press(0, 4);
    f.clock += StepSequencer::holdMs;
    f.surface.release(0, 4);
    CHECK(f.notes().size() == 1);
    undo.undo(); // the removal
    CHECK(f.notes().size() == 2);
    undo.undo(); // the second step
    CHECK(f.notes().size() == 1);
    undo.undo();
    CHECK(!f.clip().isValid());
}

TEST_CASE("Drum layout: with only the track selected, steps go into its first empty slot, which gets selected", "[control][steps]")
{
    // Clicking the track's header selects the track, not a slot: the first step's clip must be
    // the one the next steps edit, and a clip already on the track must stay.
    Fixture f;
    auto& set = f.session.set();
    set.createClip(0, 0, 4.0);
    set.addNote(f.clip(0, 0), {36, 0.0, 0.25, 100, false}, false);
    f.selection.set({Kind::track, 0, -1});
    f.refresh();
    f.tapPad(0, 0);
    f.tapPad(0, 4);
    f.tapPad(1, 0);
    CHECK(f.notes(0, 0).size() == 1); // untouched
    REQUIRE(f.clip(0, 1).isValid());
    CHECK(f.notes(0, 1).size() == 3);
    CHECK(f.selection.get() == ui::SessionSelection{Kind::slot, 0, 1});
    CHECK(f.mode().sequencer().heldSteps().empty());
    CHECK(f.surface.pad(0, 4) == LedState::solid(trackRgb));
}

TEST_CASE("Drum layout: the loop selector loops pages, Shift only shows one; the pads move a page or a row", "[control][steps]")
{
    Fixture f;
    f.tapPad(0, 0); // makes a clip a page (8 quarters) long
    REQUIRE(f.clip().isValid());
    const auto& colours = f.controller->colours();
    CHECK(f.surface.pad(4, 4) == LedState::solid(colours.white)); // the page shown
    CHECK(f.surface.pad(4, 5) != LedState::solid(trackRgb));      // past the loop

    // A page loops it; holding one and pressing another loops from one to the other.
    f.tapPad(4, 5);
    CHECK_THAT(static_cast<double>(f.clip()[ids::loopStart]), WithinAbs(8.0, 1.0e-9));
    CHECK_THAT(static_cast<double>(f.clip()[ids::loopEnd]), WithinAbs(16.0, 1.0e-9));
    StepSequencer::Target target = f.mode().sequencerTarget();
    CHECK(f.mode().sequencer().page(target) == 1);
    CHECK(f.surface.pad(4, 5) == LedState::solid(colours.white));
    f.surface.press(4, 4);
    f.surface.press(4, 6);
    f.surface.release(4, 6);
    f.surface.release(4, 4);
    f.refresh();
    CHECK_THAT(static_cast<double>(f.clip()[ids::loopStart]), WithinAbs(0.0, 1.0e-9));
    CHECK_THAT(static_cast<double>(f.clip()[ids::loopEnd]), WithinAbs(24.0, 1.0e-9));
    CHECK(f.surface.pad(4, 5) == LedState::solid(trackRgb)); // in the loop
    CHECK(f.surface.pad(4, 7) != LedState::solid(trackRgb));

    // With Shift: shown, the loop left alone; the steps are that page's.
    f.surface.down(Role::shift);
    f.tapPad(4, 6);
    f.surface.up(Role::shift);
    CHECK_THAT(static_cast<double>(f.clip()[ids::loopEnd]), WithinAbs(24.0, 1.0e-9));
    target = f.mode().sequencerTarget();
    CHECK(f.mode().sequencer().page(target) == 2);
    f.tapPad(0, 1);
    CHECK(f.notes().back().start == 16.0 + 0.25);

    // The pads move a page (16), or a row (4) with Shift; the Device view's first pad follows.
    f.surface.click(Role::octaveUp);
    CHECK(f.mode().drumStart() == 52);
    CHECK(static_cast<int>(f.rack[ids::padStart]) == 52);
    f.surface.down(Role::shift);
    f.surface.click(Role::octaveDown);
    f.surface.up(Role::shift);
    CHECK(f.mode().drumStart() == 48);

    // Layout: the pads fill the grid.
    f.surface.click(Role::layout);
    f.refresh();
    CHECK(f.mode().zones().pads.count() == 64);
    CHECK(f.controller->padMap().pads[0].kind == PadNoteMap::Pad::Kind::note);
}

TEST_CASE("Held steps take the encoders: velocity, length, nudge, one undo step a turn; the display shows them", "[control][steps]")
{
    Fixture f(presets::push2());
    auto& undo = f.session.undoManager();
    f.tapPad(0, 0);
    REQUIRE(f.notes().size() == 1);
    CHECK(f.controller->encoderModeKind() == ModeKind::device);
    undo.clearUndoHistory();

    f.surface.press(0, 0);
    f.refresh();
    CHECK(f.controller->encoderBank().target(0).startsWith("notes:"));
    CHECK(f.surface.display.title == "Step 1");
    REQUIRE(f.surface.display.cells.size() >= 3);
    CHECK(f.surface.display.cells[0].name == "Velocity");
    CHECK(f.surface.display.cells[1].name == "Length");
    CHECK(f.surface.display.cells[2].name == "Nudge");
    f.surface.touch(0);
    f.surface.turn(0, -40);
    f.surface.untouch(0);
    CHECK(f.notes()[0].velocity < steps::defaultVelocity);
    CHECK(undo.getUndoDescriptions().size() == 1);
    f.clock += 10; // let go at once, but edited: it stays
    f.surface.release(0, 0);
    f.refresh();
    CHECK(f.notes().size() == 1);
    CHECK(!f.controller->encoderBank().target(0).startsWith("notes:"));
    undo.undo();
    CHECK(f.notes()[0].velocity == steps::defaultVelocity);
}

TEST_CASE("Melodic sequencer: Layout reaches it; rows are the tuning's notes, octaves move them", "[control][steps]")
{
    Fixture f;
    f.selection.set({Kind::slot, 1, 0});
    f.refresh();
    REQUIRE(f.mode().track() == 1);
    REQUIRE(f.mode().pads() == NoteMode::Pads::play);
    f.surface.click(Role::layout); // Colundi has one layout: the next is the sequencer
    f.refresh();
    REQUIRE(f.mode().pads() == NoteMode::Pads::sequencer);
    const auto l = f.mode().layout();
    CHECK(f.mode().zones().steps.rows == 7);
    const auto target = f.mode().sequencerTarget();
    REQUIRE(target.pitches.size() == 7);
    for (int k = 0; k < 7; ++k)
        CHECK(target.pitches[static_cast<std::size_t>(k)] == l.origin + k); // a table entry a row
    CHECK(f.surface.label(6, 0).isNotEmpty()); // the bottom row's note

    // The bottom row of steps, third column: the lowest note, at the third sixteenth.
    f.tapPad(6, 2);
    REQUIRE(f.notes(1, 0).size() == 1);
    CHECK(f.notes(1, 0)[0].pitch == l.origin);
    CHECK(f.notes(1, 0)[0].start == 0.5);

    // Octave up: the rows move by the rows shown (a table); the arrows by one.
    f.surface.click(Role::octaveUp);
    CHECK(f.mode().sequencerTarget().pitches[0] == l.origin + 7);
    f.surface.click(Role::navDown);
    CHECK(f.mode().sequencerTarget().pitches[0] == l.origin + 6);

    // The step's length from the Scale menu: 1/8 makes the third step the second eighth.
    auto next = f.mode().layout();
    next.stepRate = 1;
    f.mode().setLayout(next);
    f.refresh();
    f.tapPad(5, 2);
    CHECK(f.notes(1, 0).back().start == 1.0);
    CHECK(f.notes(1, 0).back().length == 0.5);

    // Layout again: back to playing.
    f.surface.click(Role::layout);
    CHECK(f.mode().pads() == NoteMode::Pads::play);
}

TEST_CASE("Step sequencer: the playhead lights its step while the clip plays", "[control][steps]")
{
    Fixture f;
    f.tapPad(0, 0);
    REQUIRE(f.clip().isValid());
    f.session.launchSlot(0, 0);
    f.session.play();
    // Into the clip by a little over two sixteenths (120 BPM: a sixteenth is 6000 samples at 48 kHz).
    for (int i = 0; i < 8; ++i)
        f.render(2048);
    f.refresh();
    const auto& play = f.feed.trackPlay(0);
    REQUIRE(play.playingClipId == LiveSet::idOf(f.clip()));
    const int playing = static_cast<int>(play.position / 0.25);
    REQUIRE(playing >= 1);
    REQUIRE(playing < 8);
    CHECK(f.surface.pad(0, playing) == LedState::solid(dimmed(f.controller->colours().play, 0.35f)));
    CHECK(f.surface.pad(0, 0) == LedState::solid(trackRgb));
}
