// The tuning tools (docs/05-m4-ui.md, 4f), headless: the Tuning view's table (what each column shows,
// sorting, filtering, copying, playing a row), the editor (a copy the track plays while it's open,
// typed cells, shift, multiply, subsets, checks, its own undo, saving and editing again), the track's
// tuning settings in the mixer and the header's menu, the commands, and snapshots.

#include "platform/Platform.h"
#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"
#include "tuning/Tet12.h"
#include "tuning/parsers/ColundiTextParser.h"
#include "ui/shell/MainShell.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace lattice;
using namespace lattice::ui;
using namespace lattice::test;
using Catch::Approx;

namespace
{
struct Fixture
{
    Fixture()
    {
        settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lattice-tuning-" + juce::Uuid().toString() + ".settings");
        juce::PropertiesFile::Options options;
        options.applicationName = "LatticeTests";
        settings = std::make_unique<juce::PropertiesFile>(settingsFile, options);
        session.newSet(2, 2);
        session.set().setTrackTuning(0, TuningRegistry::colundiId);
        session.set().setTrackTuning(1, TuningRegistry::colundiId);
        session.flush();
        shell = std::make_unique<MainShell>(session, engine, *settings, tunings, ShellActions{});
        // Questions are recorded and answered here, never shown (a dialog left on the desktop
        // would take later tests' keys).
        shell->deviceContext().confirm = [this](const juce::String& title, const juce::String&, const juce::String& yes,
                                                std::function<void()> then) {
            questions.add(title);
            if (yes.isNotEmpty() && answerYes && then)
                then();
        };
        shell->setSize(1280, 900);
        shell->selection().set({SessionSelection::Kind::track, 0, 0});
        shell->showTuningView();
        view().flushUpdates();
        shell->userTuningsFolder().deleteRecursively();
    }
    ~Fixture()
    {
        // Saved tunings would set later tests' libraries scanning (the folder is watched).
        if (shell != nullptr)
            shell->userTuningsFolder().deleteRecursively();
        shell.reset();
        settings.reset();
        settingsFile.deleteFile();
    }

    TuningView& view() { return shell->tuningView(); }
    TrackTuning& trackTuning(int t) { return session.processorForTrack(t)->tuning(); }
    void render(std::int64_t samples)
    {
        session.flush();
        renderer.renderFor(samples);
        session.processEngineEvents();
    }
    TuningPtr colundi() const { return tunings.findById(TuningRegistry::colundiId); }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer{engine, BlockPattern::Fixed64};
    juce::File settingsFile;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<MainShell> shell;
    juce::StringArray questions;
    bool answerYes = true;
};

using Column = TuningView::Column;
} // namespace

TEST_CASE("Tuning view: the selected track's tuning, every entry and its columns", "[ui][tuning]")
{
    Fixture f;
    auto& v = f.view();
    CHECK(f.shell->detailView().page() == DetailView::Page::tuning);
    CHECK(v.track() == 0);
    CHECK(v.shown() == f.colundi());
    REQUIRE(v.rows().size() == 128);
    CHECK(v.rows().front() == 0);
    CHECK(v.cellText(47, Column::index) == "47");
    CHECK(v.cellText(47, Column::hz) == "440.000");
    CHECK(v.cellText(47, Column::ratio) == "14.6667");
    CHECK(v.cellText(47, Column::cents) == juce::String(1200.0 * std::log2(440.0 / 30.0), 1));
    CHECK(v.cellText(47, Column::label) == f.colundi()->labels[47]);
    CHECK(v.cellText(47, Column::playable) == "Yes");

    // At 48 kHz (no device: the default), the top entries are past the limit: muted, or folded.
    CHECK(v.cellText(127, Column::playable) == "Muted (too high)");
    f.session.set().setTrackHighNotePolicy(0, 1);
    v.flushUpdates();
    CHECK(v.cellText(127, Column::playable).startsWith("Folds to "));

    // Another track: it follows the selection.
    f.session.set().setTrackTuning(1, Tet12::builtinId);
    f.shell->selection().set({SessionSelection::Kind::track, 1, 0});
    v.flushUpdates();
    CHECK(v.track() == 1);
    CHECK(v.cellText(69, Column::label) == "A4");
    CHECK(v.trackControls() != nullptr);
    CHECK(v.trackControls()->track() == 1);
}

TEST_CASE("Tuning view: sorting, filtering, anchors, playable, copying", "[ui][tuning]")
{
    Fixture f;
    auto& v = f.view();
    v.sortBy(Column::hz, false);
    CHECK(v.rows().front() == 127);
    v.sortBy(Column::index, true);
    CHECK(v.rows().front() == 0);

    v.setFilter("440");
    CHECK(v.rows() == std::vector<int>{47, 127}); // 440 Hz and 44098.56 Hz
    v.setFilter("440.0");
    REQUIRE(v.rows().size() == 1);
    CHECK(v.rows()[0] == 47);
    v.setFilter("4");
    CHECK(v.rows().size() > 10); // 4, 40-49, and frequencies starting with 4
    v.setFilter({});

    v.setAnchorsOnly(true);
    for (const int e : v.rows())
        CHECK(f.colundi()->isAnchor(e));
    CHECK(v.rows().size() == tuning_edits::anchorIndices(*f.colundi()).size());
    v.setAnchorsOnly(false);

    v.setPlayableOnly(true);
    for (const int e : v.rows())
        CHECK(f.colundi()->hz[static_cast<std::size_t>(e)] < 48000.0 * 0.45);
    CHECK(v.rows().size() < 128);
    v.setPlayableOnly(false);

    const auto all = v.tableText(false);
    CHECK(all.startsWith("#\tHz\tCents\tRatio\tLabel\tPlays\n"));
    CHECK(juce::StringArray::fromLines(all.trimEnd()).size() == 129);
    v.selectEntries({47, 50});
    const auto chosen = juce::StringArray::fromLines(v.tableText(true).trimEnd());
    REQUIRE(chosen.size() == 3);
    CHECK(chosen[1].startsWith("47\t440.000\t"));
    CHECK(chosen[2].startsWith("50\t528.000\t"));
}

TEST_CASE("Tuning view: a row plays on the track while it's held, through its note offset", "[ui][tuning]")
{
    Fixture f;
    auto& v = f.view();
    v.playEntry(47, 100);
    f.render(256);
    CHECK(f.session.lastNoteOn(0) == 47);
    v.playEntry(-1, 0);

    f.session.set().setTrackNoteOffset(0, 3); // key k plays entry k + 3
    v.flushUpdates();
    v.playEntry(50, 100);
    f.render(256);
    CHECK(f.session.lastNoteOn(0) == 47);
    v.playEntry(-1, 0);
}

TEST_CASE("Tuning editor: a copy the track plays while it's open", "[ui][tuning]")
{
    Fixture f;
    auto& v = f.view();
    v.duplicate();
    REQUIRE(v.isEditing());
    CHECK(v.editDraft().name() == "Colundi copy");
    f.session.flush();
    CHECK(f.trackTuning(0).getTuning() == v.editDraft().table());
    CHECK(f.trackTuning(1).getTuning() == f.colundi()); // only its track

    SECTION("typed cells")
    {
        CHECK(v.setCell(47, Column::hz, "441"));
        f.session.flush();
        CHECK(f.trackTuning(0).hzFor(47) == Approx(441.0));
        CHECK(v.setCell(47, Column::label, "A, a little sharp"));
        CHECK(v.shown()->labels[47] == "A, a little sharp");
        CHECK(v.setCell(48, Column::hz, "0.45k"));
        CHECK(v.shown()->hz[48] == Approx(450.0));
        CHECK(!v.setCell(48, Column::hz, "abc"));
        CHECK(v.setCell(48, Column::hz, "-"));
        CHECK(v.shown()->hz[48] == 0.0);
        CHECK(v.shown()->labels[48] == "#48 (unmapped)");
    }
    SECTION("a frequency that doesn't rise: an error, no saving, and the track keeps the last playable copy")
    {
        CHECK(v.setCell(47, Column::hz, "441"));
        f.session.flush();
        const auto playable = f.trackTuning(0).getTuning();
        CHECK(v.setCell(47, Column::hz, "10"));
        CHECK(!v.problems().ok());
        CHECK(!v.saveButton().isEnabled());
        CHECK(!v.save());
        CHECK(f.questions == juce::StringArray{"Can't save the tuning"});
        f.session.flush();
        CHECK(f.trackTuning(0).getTuning() == playable);
    }
    SECTION("shift, multiply")
    {
        v.shiftBy(2);
        CHECK(v.shown()->hz[49] == Approx(440.0));
        CHECK(v.shown()->hz[0] == 0.0);
        CHECK(v.multiplyBy("3/2"));
        CHECK(v.shown()->hz[49] == Approx(660.0));
        CHECK(!v.multiplyBy("three halves"));
    }
    SECTION("keep anchors, keep selected, unmap")
    {
        v.keepAnchors();
        const auto anchors = tuning_edits::anchorIndices(*f.colundi()).size();
        int mapped = 0;
        for (const double hz : v.shown()->hz)
            mapped += hz > 0.0 ? 1 : 0;
        CHECK(mapped == static_cast<int>(anchors));
        CHECK(v.shown()->hz[static_cast<std::size_t>(tuning_edits::centredStart(static_cast<int>(anchors)))] == Approx(30.0));
        v.selectEntries({v.rows()[60], v.rows()[61]});
        v.keepSelected();
        mapped = 0;
        for (const double hz : v.shown()->hz)
            mapped += hz > 0.0 ? 1 : 0;
        CHECK(mapped == 2);
        v.selectEntries({59});
        REQUIRE(v.canEdit("edit.delete"));
        v.edit("edit.delete");
        CHECK(v.shown()->hz[59] == 0.0);
    }
    SECTION("its own undo: each edit, not the set's")
    {
        const auto setUndo = f.session.undoManager().getNumActionsInCurrentTransaction();
        CHECK(v.setCell(47, Column::hz, "441"));
        v.shiftBy(1);
        REQUIRE(v.canEdit("edit.undo"));
        v.edit("edit.undo");
        CHECK(v.shown()->hz[47] == Approx(441.0));
        v.edit("edit.undo");
        CHECK(v.shown()->hz[47] == Approx(440.0));
        CHECK(!v.editDraft().isModified());
        CHECK(!v.canEdit("edit.undo"));
        v.edit("edit.redo");
        CHECK(v.shown()->hz[47] == Approx(441.0));
        CHECK(f.session.undoManager().getNumActionsInCurrentTransaction() == setUndo);
    }
    SECTION("discarding: asked first once it's changed; the track plays its own tuning again")
    {
        CHECK(v.setCell(47, Column::hz, "441"));
        f.answerYes = false;
        v.discard();
        CHECK(f.questions == juce::StringArray{"Discard the changes?"});
        CHECK(v.isEditing());
        f.answerYes = true;
        v.discard();
        CHECK(!v.isEditing());
        f.session.flush();
        CHECK(f.trackTuning(0).getTuning() == f.colundi());
        CHECK(v.shown() == f.colundi());
    }
    SECTION("the copy stays with its track when the selection moves")
    {
        f.shell->selection().set({SessionSelection::Kind::track, 1, 0});
        v.flushUpdates();
        CHECK(v.track() == 0);
        v.discard(true);
        v.flushUpdates();
        CHECK(v.track() == 1);
    }
    SECTION("deleting its track drops the copy")
    {
        f.session.set().deleteTrack(0);
        v.flushUpdates();
        CHECK(!v.isEditing());
    }
    SECTION("a new set drops the copy (its track's ID is another track's there)")
    {
        f.session.newSet(2, 2);
        v.flushUpdates();
        CHECK(!v.isEditing());
        f.session.flush();
        CHECK(f.trackTuning(0).getTuning() == f.tunings.defaultTuning());
    }
}

TEST_CASE("Tuning editor: saving, using it, reloading it, editing it again", "[ui][tuning]")
{
    Fixture f;
    auto& v = f.view();
    v.duplicate();
    v.editDraft().setName("My Colundi");
    CHECK(v.setCell(47, Column::hz, "441"));
    CHECK(v.setCell(47, Column::label, "Sharp A"));
    REQUIRE(v.save());
    CHECK(!v.isEditing());

    // The file, in the User Library's Tunings folder, in the Colundi format.
    const auto file = f.shell->userTuningsFolder().getChildFile("My Colundi.txt");
    REQUIRE(file.existsAsFile());
    ColundiTextParser::Options options;
    options.id = "check";
    const auto back = ColundiTextParser::parse(file.loadFileAsString(), options);
    REQUIRE(back.ok());
    CHECK(back.table->hz[47] == Approx(441.0));
    CHECK(back.table->labels[47] == "Sharp A");
    CHECK(back.table->periodRatio == Approx(f.colundi()->periodRatio));

    // The track uses it (one undo step); the other track keeps Colundi.
    auto& set = f.session.set();
    const auto saved = f.tunings.findById(set.track(0)[ids::tuningId].toString());
    REQUIRE(saved != nullptr);
    CHECK(saved->name == "My Colundi");
    CHECK(saved->hz[47] == Approx(441.0));
    CHECK(set.track(1)[ids::tuningId].toString() == TuningRegistry::colundiId);
    f.session.flush();
    CHECK(f.trackTuning(0).getTuning() == saved);
    f.session.undoManager().undo();
    CHECK(set.track(0)[ids::tuningId].toString() == TuningRegistry::colundiId);
    f.session.undoManager().redo();
    CHECK(set.track(0)[ids::tuningId].toString() == saved->id);

    // A new registry (the next start) finds it in the folder.
    TuningRegistry fresh;
    CHECK(fresh.loadFolder(f.shell->userTuningsFolder()) == 1);

    // Edit it again: the same file, and every track that had it gets the new version.
    set.setTrackTuning(1, saved->id);
    v.flushUpdates();
    REQUIRE(v.canEditInPlace());
    const auto listed = f.tunings.getAll().size();
    v.editInPlace();
    REQUIRE(v.isEditing());
    CHECK(v.editDraft().name() == "My Colundi");
    CHECK(v.multiplyBy("2"));
    REQUIRE(v.save());
    const auto second = f.tunings.findById(set.track(0)[ids::tuningId].toString());
    REQUIRE(second != nullptr);
    CHECK(second != saved);
    CHECK(second->hz[47] == Approx(882.0));
    CHECK(set.track(1)[ids::tuningId].toString() == second->id);
    CHECK(f.tunings.getAll().size() == listed); // the new version took the old one's place
    CHECK(f.tunings.findById(saved->id) == saved); // which undo can still go back to
    CHECK(f.shell->userTuningsFolder().findChildFiles(juce::File::findFiles, false).size() == 1);

    // The track's tuning box lists it.
    auto* box = v.trackControls()->tuningBox();
    REQUIRE(box != nullptr);
    CHECK(box->binding().text() == "My Colundi");
}

TEST_CASE("Tuning settings: in the mixer's I/O section and the header's menu", "[ui][tuning]")
{
    Fixture f;
    auto& set = f.session.set();
    auto* strip = f.shell->sessionView().stripForColumn(0);
    REQUIRE(strip != nullptr);
    auto* controls = strip->tuningControls();
    REQUIRE(controls != nullptr);
    CHECK(controls->tuningBox()->binding().text() == "Colundi");
    controls->tuningBox()->binding().setFromText("12-TET A440");
    CHECK(set.track(0)[ids::tuningId].toString() == Tet12::builtinId);

    controls->highNotesBox()->binding().setFromText("Fold Down");
    CHECK(static_cast<int>(set.track(0)[ids::highNotePolicy]) == 1);

    CHECK(controls->bendRangeBox()->binding().spec().unit == ParamUnit::Cents);
    controls->bendRangeBox()->binding().setValue(700.0f);
    CHECK(LiveSet::bendRangeCentsOf(set.track(0)) == Approx(700.0));
    controls->bendModeBox()->binding().setFromText("Steps");
    CHECK(LiveSet::bendModeOf(set.track(0)) == 1);
    CHECK(controls->bendRangeBox()->binding().spec().unit == ParamUnit::TableSteps);
    CHECK(controls->bendRangeBox()->isVisible());
    controls->bendRangeBox()->binding().setValue(3.5f);
    f.session.flush();
    double steps = 0.0, cents = 0.0;
    f.trackTuning(0).bendOffsets(1.0f, steps, cents);
    CHECK(steps == Approx(3.5));

    // The header's tuning badge: the tunings, then the rest of the settings.
    const auto menu = f.shell->sessionView().tuningMenu(0);
    juce::StringArray items;
    for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
        items.add(it.getItem().text);
    for (const auto* expected : {"12-TET A440", "Colundi", "High Notes", "Pitch Bend", "Load Tuning File...", "Show in Tuning View"})
        CHECK(items.contains(expected));

    // A group has no tuning settings in its strip.
    set.groupTracks(0, 0);
    f.shell->sessionView().flushUpdates();
    for (int c = 0; c < 3; ++c)
        if (auto* s = f.shell->sessionView().stripForColumn(c); s != nullptr && s->kind() == MixerStrip::Kind::track &&
                                                                    set.isGroupTrack(s->index()))
            CHECK(s->tuningControls() == nullptr);
}

TEST_CASE("Tuning commands: the view, and the editor's", "[ui][tuning]")
{
    Fixture f;
    auto& commands = f.shell->commands();
    f.shell->detailView().show(DetailView::Page::clip);
    CHECK(commands.keyMappings().keyPressed(juce::KeyPress('t', juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier, 0),
                                            f.shell.get()));
    CHECK(f.shell->detailView().page() == DetailView::Page::tuning);

    CHECK(!commands.isEnabled("tuning.save"));
    CHECK(commands.perform("tuning.duplicate"));
    CHECK(f.view().isEditing());
    CHECK(commands.isEnabled("tuning.save"));
    CHECK(!commands.isEnabled("tuning.duplicate"));
    CHECK(commands.perform("tuning.discard")); // nothing changed: no question
    CHECK(!f.view().isEditing());
}

TEST_CASE("Tuning view snapshot: in every theme at 100% and 200%", "[.ui-snapshot]")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable("LATTICE_SNAPSHOT_DIR", {});
    if (dir.isEmpty())
        return;
    Fixture f;
    auto& v = f.view();
    for (const auto* page : {"inspect", "edit"})
    {
        if (juce::String(page) == "edit")
        {
            v.duplicate();
            v.setCell(46, Column::hz, "500");
            v.selectEntries({47});
        }
        for (const auto* name : {"Dark", "Light", "High Contrast"})
        {
            f.shell->setTheme(*Theme::builtIn(name));
            for (const float scale : {1.0f, 2.0f})
            {
                v.setSize(1100, 360);
                const auto image = v.createComponentSnapshot(v.getLocalBounds(), true, scale);
                const auto file = juce::File(dir).getChildFile("tuning-" + juce::String(page) + "-" + juce::String(name).replace(" ", "") +
                                                               "-" + juce::String(juce::roundToInt(scale * 100)) + ".png");
                file.deleteFile();
                juce::FileOutputStream out(file);
                juce::PNGImageFormat png;
                REQUIRE(png.writeImageToStream(image, out));
            }
        }
    }
    // The mixer with its tuning rows.
    f.shell->setTheme(Theme::dark());
    auto& session = f.shell->sessionView();
    session.setSize(900, 800);
    const auto image = session.createComponentSnapshot(session.getLocalBounds(), true, 1.0f);
    const auto file = juce::File(dir).getChildFile("tuning-mixer-Dark-100.png");
    file.deleteFile();
    juce::FileOutputStream out(file);
    juce::PNGImageFormat png;
    REQUIRE(png.writeImageToStream(image, out));
}
