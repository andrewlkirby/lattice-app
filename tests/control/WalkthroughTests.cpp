// A walkthrough of 5d's hands-on check, clicking Virtual Push in a real window as a person would
// (docs/06-m5-controllers.md, 5d): loading a kit from Browse mode, the Drum layout's steps and
// loop selector, a held step's values, the melodic sequencer, Clip mode, and both again without
// encoders. Hidden ([.]): run it by its tag; LATTICE_WALKTHROUGH_DIR saves an image at each step.

#include "support/OfflineRenderer.h"
#include "support/Snapshots.h"
#include "support/TestDevices.h"
#include "control/modes/BrowseMode.h"
#include "control/modes/ClipMode.h"
#include "control/modes/NoteMode.h"
#include "control/surface/SurfaceController.h"
#include "control/virtual/VirtualPush.h"
#include "ui/prefs/AppSettings.h"
#include "ui/shell/MainShell.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

using namespace lattice;
using namespace lattice::control;
using namespace lattice::test;
using Kind = ui::SessionSelection::Kind;

namespace
{
juce::MouseEvent mouseAt(juce::Component& c, juce::Point<float> p, juce::ModifierKeys mods = {})
{
    const auto mouse = juce::Desktop::getInstance().getMainMouseSource();
    return juce::MouseEvent(mouse, p, mods, juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, &c, &c,
                            juce::Time::getCurrentTime(), p, juce::Time::getCurrentTime(), 1, false);
}

struct Walk
{
    explicit Walk(int encoders)
    {
        settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("lattice-walk", ".settings");
        juce::PropertiesFile::Options options;
        options.applicationName = "LatticeTests";
        settings = std::make_unique<juce::PropertiesFile>(settingsFile, options);
        settings->setValue(ui::prefs::virtualColumnsKey, 12);
        settings->setValue(ui::prefs::virtualEncodersKey, encoders);
        session.newSet(2, 4); // as File > New: track 1 armed, in Colundi (the user's default)
        for (int t = 0; t < 2; ++t)
            session.set().setTrackTuning(t, TuningRegistry::colundiId);
        session.flush();
        shell = std::make_unique<ui::MainShell>(session, engine, *settings, tunings, ui::ShellActions{});
        shell->setSize(2000, 1100);
        shell->setBrowserVisible(true);
        shell->setDetailVisible(true);
        shell->deviceContext().confirm = [](const juce::String&, const juce::String&, const juce::String&, std::function<void()> ok) {
            if (ok)
                ok();
        };
        REQUIRE(shell->commands().perform("view.virtualPush"));
        prefix = encoders == 0 ? "noenc-" : "enc-";
        settle();
    }
    ~Walk()
    {
        shell.reset();
        settings.reset();
        settingsFile.deleteFile();
    }

    VirtualPush& push() { return *shell->virtualPushView(); }
    SurfaceController& controller() { return *shell->virtualPushController(); }
    LiveSet& set() { return session.set(); }
    ui::BrowserModel& browser() { return shell->browserView().model(); }

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
        shell->sessionView().flushUpdates();
        shell->deviceView().flushUpdates();
        shell->clipView().flushUpdates();
        controller().refresh();
    }
    void render(std::int64_t samples)
    {
        session.flush();
        renderer.renderFor(samples);
        session.processEngineEvents();
        now += 1.0 / 60.0;
        shell->feed().tick(now);
        controller().refresh();
    }

    juce::Point<float> padAt(int r, int c) { return push().layout().pads[static_cast<std::size_t>(r * push().capabilities().columns + c)].getCentre(); }
    juce::Point<float> buttonAt(Role role, int index = 0)
    {
        const int b = push().capabilities().indexOf({role, index});
        REQUIRE(b >= 0);
        return push().layout().buttons[static_cast<std::size_t>(b)].bounds.getCentre();
    }
    void clickAt(juce::Point<float> p)
    {
        push().mouseDown(mouseAt(push(), p));
        push().mouseUp(mouseAt(push(), p));
        settle();
    }
    void rightClickAt(juce::Point<float> p)
    {
        push().mouseDown(mouseAt(push(), p, juce::ModifierKeys::rightButtonModifier));
        push().mouseUp(mouseAt(push(), p, juce::ModifierKeys::rightButtonModifier));
        settle();
    }
    void pad(int r, int c) { clickAt(padAt(r, c)); }
    void button(Role role, int index = 0) { clickAt(buttonAt(role, index)); }
    void dragEncoder(int e, float up)
    {
        const auto at = push().layout().encoders[static_cast<std::size_t>(e)].getCentre();
        push().mouseDown(mouseAt(push(), at));
        for (int i = 1; i <= 10; ++i)
            push().mouseDrag(mouseAt(push(), at.translated(0.0f, -up * static_cast<float>(i) / 10.0f)));
        push().mouseUp(mouseAt(push(), at.translated(0.0f, -up)));
        settle();
    }
    /** Finds a browser row by name, opening rows until it shows. */
    int row(const juce::String& name)
    {
        for (int pass = 0; pass < 4; ++pass)
        {
            const auto& rows = browser().rows();
            for (std::size_t r = 0; r < rows.size(); ++r)
                if (rows[r].item.name == name)
                    return static_cast<int>(r);
            bool opened = false;
            for (std::size_t r = 0; r < rows.size() && !opened; ++r)
                if (rows[r].expandable && !rows[r].expanded)
                {
                    browser().setExpanded(static_cast<int>(r), true);
                    opened = true;
                }
            settle();
            if (!opened)
                break;
        }
        return -1;
    }
    void shot(const juce::String& name)
    {
        const auto dir = juce::SystemStats::getEnvironmentVariable("LATTICE_WALKTHROUGH_DIR", {});
        if (dir.isEmpty())
            return;
        push().repaint();
        writePng(snapshot(push(), 1.0f), juce::File(dir).getChildFile(prefix + name + ".png"));
        writePng(snapshot(*shell, 0.6f), juce::File(dir).getChildFile(prefix + name + "-window.png"));
    }
    juce::String status() { return push().text().status; }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer{engine, BlockPattern::Fixed64};
    juce::File settingsFile;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<ui::MainShell> shell;
    juce::String prefix;
    double now = 3.0;
};
} // namespace

TEST_CASE("Walkthrough: 5d on Virtual Push, 8 x 12, as a person clicks it", "[.walkthrough]")
{
    const int encoders = GENERATE(0, 8);
    INFO("encoders: " << encoders);
    Walk w(encoders);
    auto& set = w.set();
    REQUIRE(w.push().capabilities().columns == 12);
    REQUIRE(w.push().capabilities().encoders == encoders);

    // The track header clicked: the track, no slot.
    w.shell->selection().set({Kind::track, 0, -1});
    w.settle();

    // --- Load the Synth Kit from Browse mode -------------------------------------------------
    w.button(Role::modeBrowse);
    w.shot("01-browse");
    CHECK((encoders > 0 ? w.controller().encoderModeKind() : w.controller().modeKind()) == ModeKind::browse);
    if (encoders > 0)
    {
        const int drums = [&] {
            const auto& upper = w.push().displayed().upper;
            for (std::size_t i = 0; i < upper.size(); ++i)
                if (upper[i].label == "Drums")
                    return static_cast<int>(i);
            return -1;
        }();
        REQUIRE(drums >= 0);
        w.button(Role::displayUpper, drums);
    }
    else
    {
        w.browser().setSource(*std::find_if(w.browser().sources().begin(), w.browser().sources().end(),
                                            [](const auto& s) { return s.name == "Drums"; }));
        w.settle();
    }
    const int kit = w.row("Synth Kit");
    REQUIRE(kit >= 0);
    w.browser().select(kit);
    w.settle();
    w.shot("02-browse-kit");
    if (encoders > 0)
        w.button(Role::displayLower, 0); // Load
    else
    {
        // The grid: a row of pads each, the last pad loads.
        const int columns = w.push().capabilities().columns;
        bool loaded = false;
        for (int r = 0; r < w.push().capabilities().rows && !loaded; ++r)
            if (w.push().text().pads[static_cast<std::size_t>(r * columns)].contains("Synth Kit"))
            {
                w.pad(r, columns - 1);
                loaded = true;
            }
        CHECK(loaded);
    }
    auto chain = LiveSet::chainOf(set.track(0));
    REQUIRE(chain.getNumChildren() >= 1);
    CHECK(chain.getChild(chain.getNumChildren() - 1)[ids::deviceType].toString() == "lattice.drumRack");

    // --- Drum layout ---------------------------------------------------------------------------
    w.button(Role::modeNote);
    CHECK(w.controller().modeKind() == ModeKind::note);
    auto& note = w.controller().noteMode();
    REQUIRE(note.pads() == NoteMode::Pads::drum);
    const auto zones = note.zones();
    INFO("steps " << zones.steps.rows << "x" << zones.steps.columns << " pads " << zones.pads.rows << "x" << zones.pads.columns
                  << " loop " << zones.loop.rows << "x" << zones.loop.columns);
    w.shot("03-note-drum");

    // The Kick (the pads' bottom-left), then four steps along the top row.
    w.pad(zones.pads.row + zones.pads.rows - 1, zones.pads.column);
    CHECK(note.drumNote() == 36);
    for (const int c : {0, 4, 8})
        w.pad(0, c);
    w.pad(1, 0);
    auto clip = set.clip(0, 0);
    REQUIRE(clip.isValid());
    CHECK(LiveSet::notesOf(clip).size() == 4);
    CHECK(w.shell->selection().get() == ui::SessionSelection{Kind::slot, 0, 0});
    CHECK(note.sequencer().heldSteps().empty());
    // The snare's steps.
    w.pad(zones.pads.row + zones.pads.rows - 1, zones.pads.column + 2);
    CHECK(note.drumNote() == 38);
    for (const int c : {2, 6, 10})
        w.pad(0, c);
    CHECK(LiveSet::notesOf(clip).size() == 7);
    w.shot("04-steps");
    // Tapped again, a step loses its note.
    w.pad(0, 10);
    CHECK(LiveSet::notesOf(clip).size() == 6);

    // Launch it: the playhead on a step.
    w.session.launchSlot(0, 0);
    for (int i = 0; i < 400; ++i)
        w.render(256);
    CHECK(w.shell->feed().slot(0, 0) == SlotState::Playing);
    w.shot("05-playing");

    // The loop selector: hold page 1, click page 2, let page 1 go.
    const double page = note.sequencer().pageLength(note.sequencerTarget());
    w.rightClickAt(w.padAt(zones.loop.row, zones.loop.column));
    w.pad(zones.loop.row, zones.loop.column + 1);
    w.pad(zones.loop.row, zones.loop.column);
    CHECK(static_cast<double>(clip[ids::loopEnd]) == page * 2);
    w.shot("06-two-pages");
    // A page alone.
    w.pad(zones.loop.row, zones.loop.column + 1);
    CHECK(static_cast<double>(clip[ids::loopStart]) == page);
    CHECK(static_cast<double>(clip[ids::loopEnd]) == page * 2);
    w.pad(zones.loop.row, zones.loop.column);
    w.session.stop();
    w.render(256);

    // Octave moves the pads; Layout shows 64 (here 96) pads, and back.
    w.button(Role::octaveUp);
    CHECK(note.drumStart() == 36 + zones.pads.count());
    w.shot("07-octave-up");
    w.button(Role::octaveDown);
    CHECK(note.drumStart() == 36);
    w.button(Role::layout);
    CHECK(note.zones().steps.empty());
    w.shot("08-layout-pads");
    w.button(Role::layout);
    CHECK(note.zones() == zones);

    // --- A held step ----------------------------------------------------------------------------
    w.pad(zones.pads.row + zones.pads.rows - 1, zones.pads.column); // the Kick
    w.rightClickAt(w.padAt(0, 4));
    CHECK(note.sequencer().heldSteps().size() == 1);
    w.shot("09-held-step");
    if (encoders > 0)
    {
        CHECK(w.push().displayed().cells[0].name == "Velocity");
        const auto before = w.session.undoManager().getUndoDescriptions().size();
        w.dragEncoder(0, -60.0f);
        const auto notes = LiveSet::notesOf(clip);
        const auto it = std::find_if(notes.begin(), notes.end(), [](const NoteData& n) { return n.pitch == 36 && n.start == 1.0; });
        REQUIRE(it != notes.end());
        CHECK(it->velocity < 100);
        CHECK(w.session.undoManager().getUndoDescriptions().size() == before + 1);
        w.shot("10-held-velocity");
    }
    juce::Thread::sleep(static_cast<int>(StepSequencer::holdMs) + 100); // a person's hold
    w.pad(0, 4); // lets it go
    CHECK(note.sequencer().heldSteps().empty());
    CHECK(LiveSet::notesOf(clip).size() == 6);

    // --- The melodic sequencer on track 2, a Colundi preset ------------------------------------
    w.shell->selection().set({Kind::track, 1, -1});
    w.settle();
    set.setTrackArm(1, true, false); // armed by hand (arming one disarms the other)
    w.button(Role::modeBrowse);
    w.browser().setSource(*std::find_if(w.browser().sources().begin(), w.browser().sources().end(),
                                        [](const auto& s) { return s.name == "Instruments"; }));
    w.settle();
    const int bass = w.row("Colundi Bass");
    REQUIRE(bass >= 0);
    w.browser().select(bass);
    w.settle();
    if (encoders > 0)
        w.button(Role::displayLower, 0);
    else
        w.shell->browseHost().load();
    w.settle();
    REQUIRE(LiveSet::chainOf(set.track(1)).getNumChildren() >= 1);
    w.button(Role::modeNote);
    CHECK(note.pads() == NoteMode::Pads::play);
    w.shot("11-colundi-play");
    w.button(Role::layout);
    CHECK(note.pads() == NoteMode::Pads::sequencer);
    INFO("status: " << w.status());
    w.shot("12-colundi-sequencer");
    w.pad(3, 0);
    w.pad(2, 2);
    w.pad(1, 4);
    auto lead = set.clip(1, 0);
    REQUIRE(lead.isValid());
    CHECK(LiveSet::notesOf(lead).size() == 3);
    w.shot("13-colundi-steps");
    // The Scale menu.
    w.button(Role::scale);
    w.shot("14-scale-menu");
    w.button(Role::scale);

    // --- Clip mode ------------------------------------------------------------------------------
    w.shell->selection().set({Kind::slot, 0, 0});
    w.settle();
    w.button(Role::modeClip);
    CHECK((encoders > 0 ? w.controller().encoderModeKind() : w.controller().modeKind()) == ModeKind::clip);
    w.shot("15-clip");
    const double end = clip[ids::loopEnd];
    if (encoders > 0)
    {
        const auto upper = w.push().displayed().upper;
        for (int b = 0; b < static_cast<int>(upper.size()); ++b)
            if (upper[static_cast<std::size_t>(b)].label.isNotEmpty())
            {
                w.button(Role::displayUpper, b);
                w.shot("16-clip-page-" + juce::String(b + 1));
            }
        w.button(Role::displayUpper, 0);
        w.button(Role::displayLower, 1); // Loop x 2
    }
    else
    {
        // The bottom row's actions: Double Loop, Loop x 2...
        const int bottom = w.push().capabilities().rows - 1;
        w.pad(bottom, 1);
    }
    CHECK(static_cast<double>(clip[ids::loopEnd]) == end * 2);
    w.shot("17-clip-loop-x2");
    w.button(Role::fixedLength);
    CHECK(static_cast<int>(set.state()[ids::fixedRecordBars]) != 0);
    w.button(Role::fixedLength);

    // --- Browse: hot-swap the kit's... the track's rack -----------------------------------------
    w.shell->selection().set({Kind::track, 1, -1});
    w.settle();
    w.button(Role::modeBrowse);
    w.shot("18-browse-again");
}
