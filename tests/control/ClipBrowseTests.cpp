// Clip and Browse modes (docs/06-m5-controllers.md, 5d; Tests 1, "Mix / Browse / Clip"): Clip
// mode's pages for 8, 16 and 32 encoders, its values as the Clip view's bindings (a turn one undo
// step), its actions and the global Double Loop, Quantize and Fixed Length buttons, the grid as
// faders and actions without encoders; Browse mode driving the window's browser (the categories,
// scrolling, Load, Open and Back, previews, hot-swap) and its grid without encoders.

#include "support/FakeSurface.h"
#include "support/TestDevices.h"
#include "control/modes/BrowseMode.h"
#include "control/modes/ClipMode.h"
#include "control/surface/Presets.h"
#include "control/surface/SurfaceController.h"
#include "ui/clip/ClipActions.h"
#include "ui/clip/ClipBindings.h"
#include "ui/commands/SessionCommands.h"
#include "ui/shell/MainShell.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace lattice;
using namespace lattice::control;
using namespace lattice::test;
using Catch::Matchers::WithinAbs;
using Kind = ui::SessionSelection::Kind;

namespace
{
struct Fixture
{
    explicit Fixture(SurfaceCapabilities caps = control::presets::push2()) : surface(std::move(caps))
    {
        session.newSet(2, 4);
        for (int t = 0; t < 2; ++t)
        {
            auto chain = LiveSet::chainOf(session.set().track(t));
            while (chain.getNumChildren() > 0)
                session.set().removeDevice(chain.getChild(0));
        }
        session.flush();
        ui::addSessionCommands(commands, session, {[this] { return selection.get(); }, [this](ui::SessionSelection s) { selection.set(s); }}, {});
        controller = std::make_unique<SurfaceController>(SurfaceContext{session, commands, selection, feed, &specs}, surface);
        selection.set({Kind::slot, 0, 0});
        controller->setMode(ModeKind::clip);
        refresh();
    }
    ~Fixture() { controller.reset(); }

    void refresh() { controller->refresh(); }
    ClipMode& mode() { return controller->clipMode(); }
    juce::ValueTree makeClip(int t = 0, int s = 0, double length = 4.0)
    {
        auto c = session.set().createClip(t, s, length);
        session.set().addNote(c, {60, 0.1, 0.5, 100, false});
        session.set().addNote(c, {62, 1.05, 0.5, 100, false});
        session.undoManager().clearUndoHistory();
        refresh();
        return c;
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    DeviceSpecs specs{testRegistry()};
    ui::EngineStateFeed feed{session, engine};
    ui::CommandRegistry commands{&session.undoManager()};
    ui::SessionSelectionModel selection;
    FakeSurface surface;
    std::unique_ptr<SurfaceController> controller;
};

juce::StringArray pageNames(ClipMode& m)
{
    juce::StringArray names;
    for (const auto& p : m.pages())
        names.add(p.name + " " + juce::String(static_cast<int>(p.values.size())));
    return names;
}
} // namespace

TEST_CASE("Clip mode: the pages fit the encoders; the values are the Clip view's, a turn one undo step", "[control][clip]")
{
    {
        Fixture f(control::presets::virtualPush(8, 8, 16));
        CHECK(pageNames(f.mode()) == juce::StringArray{juce::String::fromUTF8("Loop \xc2\xb7 Launch \xc2\xb7 Signature 14"), "Quantize 3"});
    }
    {
        Fixture f(control::presets::virtualPush(8, 8, 32));
        CHECK(pageNames(f.mode()) == juce::StringArray{"Clip 17"});
    }
    Fixture f;
    CHECK(pageNames(f.mode()) == juce::StringArray{"Loop 5", "Launch 7", juce::String::fromUTF8("Signature \xc2\xb7 Quantize 5")});
    CHECK(f.controller->encoderModeKind() == ModeKind::clip);
    CHECK(f.surface.button(Role::modeClip) == LedState::solid(f.controller->colours().white));

    // No clip: nothing to edit, and the display says so.
    CHECK(f.surface.display.message == "No clip selected");
    CHECK(f.controller->encoderBank().target(0).isEmpty());

    auto c = f.makeClip();
    REQUIRE(f.controller->encoderBank().target(3) == ui::clipValues::target(c, "loopLength"));
    CHECK(f.surface.display.cells[3].name == "Loop Length");
    CHECK(f.surface.display.title.contains("Clip"));
    CHECK(f.surface.display.upper[0].label == "Loop");
    CHECK(f.surface.display.lower[0].label == "Double Loop");

    // A turn: the loop longer, one undo step (the panel's binding, so the panel's undo name).
    auto& undo = f.session.undoManager();
    f.surface.touch(3);
    for (int i = 0; i < 4; ++i)
        f.surface.turn(3, 5);
    f.surface.untouch(3);
    CHECK(static_cast<double>(c[ids::loopEnd]) > 4.0);
    CHECK(undo.getUndoDescriptions().size() == 1);
    CHECK(undo.getUndoDescriptions()[0].startsWith("Loop Length"));
    undo.undo();
    CHECK_THAT(static_cast<double>(c[ids::loopEnd]), WithinAbs(4.0, 1.0e-9));

    // The upper buttons choose the page: Launch's first encoder is the launch mode.
    f.surface.click(Role::displayUpper, 1);
    f.refresh();
    CHECK(f.mode().page() == 1);
    CHECK(f.controller->encoderBank().target(0) == ui::clipValues::target(c, "launchMode"));
    f.surface.touch(0);
    f.surface.turn(0, EncoderBank::stepsPerSetting);
    f.surface.untouch(0);
    CHECK(static_cast<int>(c[ids::launchMode]) == 1); // Gate

    // The Quantize page's values are the set's, there with or without a clip.
    f.surface.click(Role::displayUpper, 2);
    f.refresh();
    CHECK(f.controller->encoderBank().target(2) == "set/quantizeGrid");
    CHECK(f.controller->encoderBank().target(4) == "set/fixedRecordBars");
}

TEST_CASE("Clip mode: Double Loop, Quantize and Duplicate; the global buttons; Fixed Length", "[control][clip]")
{
    Fixture f;
    auto c = f.makeClip();
    auto& set = f.session.set();
    auto& undo = f.session.undoManager();

    // Double Loop: the notes copied after the loop, the loop twice as long; one undo step.
    f.surface.click(Role::displayLower, 0);
    CHECK_THAT(static_cast<double>(c[ids::loopEnd]), WithinAbs(8.0, 1.0e-9));
    CHECK(LiveSet::notesOf(c).size() == 4);
    CHECK(undo.getUndoDescriptions().size() == 1);
    f.surface.click(Role::displayLower, 2); // Loop / 2
    CHECK_THAT(static_cast<double>(c[ids::loopEnd]), WithinAbs(4.0, 1.0e-9));
    f.surface.click(Role::displayLower, 1); // Loop x2: the notes stay
    CHECK_THAT(static_cast<double>(c[ids::loopEnd]), WithinAbs(8.0, 1.0e-9));
    CHECK(LiveSet::notesOf(c).size() == 4);

    // Quantize: by the set's grid and amount (1/16 at 100 % unless set).
    CHECK(ui::clipActions::quantizeGridOf(set) == ui::clipActions::defaultGrid);
    f.surface.click(Role::displayLower, 3);
    CHECK_THAT(LiveSet::notesOf(c)[0].start, WithinAbs(0.0, 1.0e-9));
    CHECK_THAT(LiveSet::notesOf(c)[1].start, WithinAbs(1.0, 1.0e-9));
    undo.undo();
    set.state().setProperty(ids::quantizeAmount, 50, nullptr);
    f.surface.click(Role::quantize); // the global button: the same
    CHECK_THAT(LiveSet::notesOf(c)[0].start, WithinAbs(0.05, 1.0e-9));

    // Duplicate: into the next slot below, which is selected.
    f.surface.click(Role::displayLower, 4);
    CHECK(set.clip(0, 1).isValid());
    CHECK(f.selection.get().scene == 1);
    f.refresh();
    CHECK(f.mode().clip() == set.clip(0, 1));

    // The global Double Loop acts on the selected clip in any mode.
    f.controller->setMode(ModeKind::device);
    const double before = set.clip(0, 1)[ids::loopEnd];
    f.surface.click(Role::doubleLoop);
    CHECK_THAT(static_cast<double>(set.clip(0, 1)[ids::loopEnd]), WithinAbs(before * 2.0, 1.0e-9));

    // Fixed Length: on (1 bar) and off; with Shift, the next length; each one undo step.
    CHECK(set.fixedRecordBars() == 0);
    f.surface.click(Role::fixedLength);
    CHECK(set.fixedRecordBars() == 1);
    f.refresh();
    CHECK(f.surface.button(Role::fixedLength) == LedState::solid(f.controller->colours().white));
    f.surface.down(Role::shift);
    f.surface.click(Role::fixedLength);
    f.surface.up(Role::shift);
    CHECK(set.fixedRecordBars() == 2);
    f.surface.click(Role::fixedLength);
    CHECK(set.fixedRecordBars() == 0);
    f.surface.click(Role::fixedLength);
    CHECK(set.fixedRecordBars() == 2); // back to the length it had
    undo.undo();
    CHECK(set.fixedRecordBars() == 0);
}

TEST_CASE("Clip mode without encoders: the columns are faders, the bottom row the actions", "[control][clip]")
{
    Fixture f(control::presets::gridOnly(8, 8));
    REQUIRE(f.controller->modeKind() == ModeKind::clip);
    auto c = f.makeClip();
    CHECK(f.mode().pages().front().values.size() == 5); // Loop fits 8 columns
    // The top of the Loop Length column: its whole range.
    f.surface.tap(0, 3);
    CHECK_THAT(f.controller->encoderBank().binding(3)->normalised(), WithinAbs(1.0, 1.0e-6));
    f.surface.tap(6, 3); // the bottom fader row: its least
    CHECK_THAT(static_cast<double>(c[ids::loopEnd]) - static_cast<double>(c[ids::loopStart]), WithinAbs(0.25, 1.0e-9));
    // The bottom row: Double Loop first.
    f.surface.tap(7, 0);
    CHECK_THAT(static_cast<double>(c[ids::loopEnd]) - static_cast<double>(c[ids::loopStart]), WithinAbs(0.5, 1.0e-9));
    f.refresh();
    CHECK(f.surface.label(7, 0) == "Double Loop");
    CHECK(!f.surface.pad(7, 0).isOff());
    // With no display, each fader's top pad names it and shows its value.
    CHECK(f.surface.label(0, 3).startsWith("Loop Length"));
    CHECK(f.surface.label(0, 3).containsChar('\n'));
}

// --- Browse mode ------------------------------------------------------------------------------------

namespace
{
struct ShellFixture
{
    explicit ShellFixture(SurfaceCapabilities caps = control::presets::push2()) : surface(std::move(caps))
    {
        settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lattice-browse-" + juce::Uuid().toString() + ".settings");
        juce::PropertiesFile::Options options;
        options.applicationName = "LatticeTests";
        settings = std::make_unique<juce::PropertiesFile>(settingsFile, options);
        session.newSet(2, 2);
        for (int t = 0; t < 2; ++t)
        {
            auto chain = LiveSet::chainOf(session.set().track(t));
            while (chain.getNumChildren() > 0)
                session.set().removeDevice(chain.getChild(0));
        }
        session.flush();
        shell = std::make_unique<ui::MainShell>(session, engine, *settings, tunings, ui::ShellActions{});
        shell->setSize(1600, 1000);
        shell->setBrowserVisible(true);
        shell->setDetailVisible(true);
        shell->deviceContext().confirm = [](const juce::String&, const juce::String&, const juce::String&, std::function<void()> ok) {
            if (ok)
                ok();
        };
        controller = std::make_unique<SurfaceController>(
            SurfaceContext{session, shell->commands(), shell->selection(), shell->feed(), nullptr, &shell->browseHost()}, surface);
        shell->selection().set({Kind::track, 0, 0});
        settle();
    }
    ~ShellFixture()
    {
        controller.reset();
        shell.reset();
        settings.reset();
        settingsFile.deleteFile();
    }
    void settle()
    {
        for (int i = 0; i < 20; ++i)
        {
            shell->jobs().waitUntilIdle(5000);
            shell->library().deliverForTests();
            const bool wasIdle = shell->jobs().idle();
            if (shell->jobs().deliverFinished() == 0 && wasIdle)
                break;
        }
        session.flush();
        controller->refresh();
    }
    ui::BrowserModel& model() { return shell->browserView().model(); }
    int rowNamed(const juce::String& name)
    {
        const auto& rows = model().rows();
        for (std::size_t r = 0; r < rows.size(); ++r)
            if (rows[r].item.name == name)
                return static_cast<int>(r);
        return -1;
    }
    juce::ValueTree chain(int t) { return LiveSet::chainOf(session.set().track(t)); }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    juce::File settingsFile;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<ui::MainShell> shell;
    FakeSurface surface;
    std::unique_ptr<SurfaceController> controller;
};
} // namespace

TEST_CASE("Browse mode: only with a browser; the categories, scrolling, Load, Open and Back, previews, hot-swap", "[control][browse]")
{
    {
        Fixture plain;
        CHECK_FALSE(plain.controller->isAvailable(ModeKind::browse));
        CHECK(plain.surface.button(Role::modeBrowse).isOff());
    }
    ShellFixture f;
    REQUIRE(f.controller->isAvailable(ModeKind::browse));
    f.controller->setMode(ModeKind::browse);
    f.controller->refresh();
    REQUIRE(f.controller->encoderModeKind() == ModeKind::browse);
    auto& display = f.surface.display;
    REQUIRE(display.lists.size() == 2);
    CHECK(display.lists[0].title == "Sources");
    CHECK(display.upper[0].label == "Instruments");

    // A category from the upper buttons; its rows on the display, the screen's browser the same.
    f.surface.click(Role::displayUpper, 0);
    f.settle();
    CHECK(f.model().source().name == "Instruments");
    REQUIRE(!f.model().rows().empty());
    CHECK(display.lists[1].title == "Instruments");

    // The encoders past the sources move along the rows: a row every stepsPerRow steps.
    f.model().select(0);
    f.settle();
    f.surface.turn(3, BrowseMode::stepsPerRow - 1);
    CHECK(f.model().selectedRow() == 0);
    f.surface.turn(3, 1);
    CHECK(f.model().selectedRow() == 1);
    f.surface.turn(3, -BrowseMode::stepsPerRow);
    CHECK(f.model().selectedRow() == 0);
    // The first encoder chooses the source.
    const auto first = f.model().source();
    f.surface.turn(0, BrowseMode::stepsPerRow);
    f.settle();
    CHECK(!(f.model().source() == first));
    f.surface.click(Role::displayUpper, 0);
    f.settle();

    // Load: the selected row goes where the Device view is looking (track 0's chain).
    const int synth = f.rowNamed("Synth");
    REQUIRE(synth >= 0);
    f.model().select(synth);
    f.settle();
    CHECK(display.lists[1].items[static_cast<std::size_t>(display.lists[1].selected)].contains("Synth"));
    f.surface.click(Role::displayLower, 0);
    f.settle();
    REQUIRE(f.chain(0).getNumChildren() == 1);
    CHECK(f.chain(0).getChild(0)[ids::deviceType].toString() == "lattice.synth");

    // Open shows a device's presets; Back closes it again.
    f.surface.click(Role::displayLower, 1);
    f.settle();
    CHECK(f.model().rows()[static_cast<std::size_t>(f.rowNamed("Synth"))].expanded);
    f.surface.click(Role::displayLower, 2);
    f.settle();
    CHECK_FALSE(f.model().rows()[static_cast<std::size_t>(f.rowNamed("Synth"))].expanded);

    // Preview on and off, as the browser's own switch.
    const bool was = f.shell->browserView().isPreviewOn();
    f.surface.click(Role::displayLower, 3);
    CHECK(f.shell->browserView().isPreviewOn() != was);
    f.surface.click(Role::displayLower, 3);
    CHECK(f.shell->browserView().isPreviewOn() == was);

    // Hot-swap the Synth: the next Load replaces it.
    f.shell->deviceView().select(f.chain(0).getChild(0));
    f.surface.click(Role::displayLower, 4);
    f.settle();
    CHECK(f.shell->deviceView().hotSwapTarget() == f.chain(0).getChild(0));
    CHECK(display.title.startsWith("Hot-swap"));
    f.model().select(f.rowNamed("Sampler"));
    f.settle();
    f.surface.click(Role::displayLower, 0);
    f.settle();
    REQUIRE(f.chain(0).getNumChildren() == 1);
    CHECK(f.chain(0).getChild(0)[ids::deviceType].toString() == "lattice.sampler");
}

TEST_CASE("Browse mode without encoders: the rows on the grid, a press previews, the last pad loads", "[control][browse]")
{
    ShellFixture f(control::presets::launchpadProMk3()); // a grid and arrows, no encoders
    f.controller->setMode(ModeKind::browse);
    REQUIRE(f.controller->modeKind() == ModeKind::browse);
    for (const auto& s : f.model().sources())
        if (s.name == "Instruments")
            f.model().setSource(s);
    f.settle();
    const int synth = f.rowNamed("Synth");
    REQUIRE(synth >= 0);
    REQUIRE(synth < 8);
    CHECK(f.surface.label(synth, 0) == "Synth");
    // The scene buttons name their categories.
    const auto& caps = f.surface.capabilities();
    REQUIRE(f.surface.text.buttons.size() == caps.buttons.size());
    CHECK(f.surface.text.buttons[static_cast<std::size_t>(caps.indexOf({Role::scene, 0}))] == "Instruments");
    CHECK(f.surface.text.buttons[static_cast<std::size_t>(caps.indexOf({Role::scene, 1}))] == "Drums");
    f.surface.tap(synth, 0);
    CHECK(f.model().selectedRow() == synth);
    f.controller->refresh();
    CHECK(f.surface.pad(synth, 1) == LedState::solid(f.controller->colours().white));
    // The arrows move along the rows here (no encoders).
    REQUIRE(synth > 0);
    f.surface.click(Role::navUp);
    CHECK(f.model().selectedRow() == synth - 1);
    f.surface.click(Role::navDown);
    CHECK(f.model().selectedRow() == synth);
    f.surface.tap(synth, 7);
    f.settle();
    REQUIRE(f.chain(0).getNumChildren() == 1);
}
