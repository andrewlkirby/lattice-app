// The browser (docs/05-m4-ui.md, 4e), headless, in the window as the app builds it: the sources and
// their rows, devices opening to their presets and folders to what's in them, search, inserting
// where the Device view is looking (M3 hands-on finding 4), every drag target (a track, a device's
// title bar, a Sampler, a pad, a tuning onto a track), previews on the preview bus, the browser
// filling 100,000 results within a frame, and snapshots.

#include "devices/racks/Rack.h"
#include "devices/racks/RackEditing.h"
#include "library/Library.h"
#include "support/TestDevices.h"
#include "support/TestFiles.h"
#include "ui/browser/BrowserView.h"
#include "ui/device/DeviceContext.h"
#include "ui/device/DeviceFrame.h"
#include "ui/shell/MainShell.h"
#include "ui/theme/Theme.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <thread>

using namespace lattice;
using namespace lattice::ui;
using namespace lattice::test;
using library::Kind;

namespace
{
struct Fixture
{
    Fixture()
    {
        settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lattice-browser-" + juce::Uuid().toString() + ".settings");
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
        shell = std::make_unique<MainShell>(session, engine, *settings, tunings, ShellActions{});
        shell->setSize(1600, 1000);
        shell->setBrowserVisible(true);
        shell->setDetailVisible(true);
        // Questions are answered at once in tests.
        shell->deviceContext().confirm = [this](const juce::String&, const juce::String& q, const juce::String& yes,
                                                std::function<void()> ok) {
            questions.add(q);
            if (answerYes && yes.isNotEmpty() && ok)
                ok();
        };
        settle();
    }
    ~Fixture()
    {
        shell.reset();
        settings.reset();
        settingsFile.deleteFile();
    }

    BrowserView& browser() { return shell->browserView(); }
    BrowserModel& model() { return browser().model(); }
    LiveSet& set() { return session.set(); }
    juce::ValueTree chain(int track) { return LiveSet::chainOf(set().track(track)); }

    /** What the message loop would do: jobs' results, the library's news, the views' updates. */
    void settle()
    {
        for (int i = 0; i < 20; ++i)
        {
            shell->jobs().waitUntilIdle(5000);
            SamplePool::shared().waitUntilIdle();
            SamplePool::shared().deliverFinished();
            shell->library().deliverForTests();
            // Idle before delivering: a job finishing in between is delivered next time round.
            const bool wasIdle = shell->jobs().idle();
            const int delivered = shell->jobs().deliverFinished();
            if (delivered == 0 && wasIdle)
                break;
        }
        session.flush();
    }

    void showSource(const BrowserModel::Source::Kind kind, const juce::String& name)
    {
        for (const auto& s : model().sources())
            if (s.kind == kind && s.name == name)
                model().setSource(s);
        settle();
    }

    int rowNamed(const juce::String& name) const
    {
        const auto& rows = shell->browserView().model().rows();
        for (std::size_t r = 0; r < rows.size(); ++r)
            if (rows[r].item.name == name)
                return static_cast<int>(r);
        return -1;
    }

    juce::DragAndDropTarget::SourceDetails dragOf(const juce::String& description, juce::Point<int> at)
    {
        return {description, &browser(), at};
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    juce::File settingsFile;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<MainShell> shell;
    juce::StringArray questions;
    bool answerYes = true;
};

juce::StringArray namesOf(const BrowserModel& m)
{
    juce::StringArray names;
    for (const auto& r : m.rows())
        names.add(r.item.name);
    return names;
}
} // namespace

TEST_CASE("Browser: the Plugins category opens on its groups, and a plugin isn't inserted yet", "[ui][browser]")
{
    Fixture f;
    // As the plugin scan sets them (docs/07-m6-plugins-external.md, 6a).
    library::Item group;
    group.kind = Kind::folder;
    group.key = "plugins:0:Some Vendor";
    group.name = "Some Vendor";
    library::Item plugin;
    plugin.kind = Kind::plugin;
    plugin.key = "plugin:VST3-Some Synth-1234-5678";
    plugin.name = "Some Synth";
    plugin.deviceType = "plugin";
    plugin.folder = group.key;
    plugin.detail = "Some Vendor \xc2\xb7 VST3 \xc2\xb7 Instrument";
    f.shell->library().index().setPluginItems({plugin, group});
    f.settle();

    f.showSource(BrowserModel::Source::Kind::category, "Plugins");
    CHECK(namesOf(f.model()) == juce::StringArray{"Some Vendor"});
    REQUIRE(f.model().rows()[0].expandable);
    f.model().setExpanded(0, true);
    f.settle();
    CHECK(namesOf(f.model()) == juce::StringArray{"Some Vendor", "Some Synth"});
    CHECK(f.model().rows()[1].depth == 1);
    CHECK_FALSE(f.model().rows()[1].expandable);
    f.model().select(1);
    CHECK_FALSE(f.browser().insertSelected()); // hosting comes in 6b

    // Plugins come up in a search, and in the Favourites.
    f.model().setSearch("some synth");
    f.settle();
    CHECK(namesOf(f.model()).contains("Some Synth"));
    f.shell->library().index().setInCollection(plugin.key, 0, true);
    f.model().setSearch({});
    f.showSource(BrowserModel::Source::Kind::collection, "Favourites");
    CHECK(namesOf(f.model()).contains("Some Synth"));
}

TEST_CASE("Browser: the sources, and each category's rows", "[ui][browser]")
{
    Fixture f;
    // Collections, then categories, then places (the User Library first).
    const auto sources = f.model().sources();
    REQUIRE(sources.size() >= 16);
    CHECK(sources[0].kind == BrowserModel::Source::Kind::collection);
    CHECK(sources[0].name == "Favourites");
    CHECK(sources[7].name == "Instruments");
    CHECK(sources[11].name == "Plugins"); // after the MIDI effects, as in Live
    CHECK(sources[14].name == "Tunings");
    CHECK(sources[15].kind == BrowserModel::Source::Kind::place);
    CHECK(sources[15].name == "User Library");

    f.showSource(BrowserModel::Source::Kind::category, "Instruments");
    auto names = namesOf(f.model());
    CHECK(names.contains("Synth"));
    CHECK(names.contains("Sampler"));
    CHECK(names.contains("Instrument Rack"));
    CHECK(names.contains("Drum Rack"));
    CHECK_FALSE(names.contains("Reverb"));
    CHECK_FALSE(names.contains("Test Sine")); // hidden test devices aren't offered

    // A device opens to show its presets.
    const int synth = f.rowNamed("Synth");
    REQUIRE(synth >= 0);
    CHECK(f.model().rows()[static_cast<std::size_t>(synth)].expandable);
    f.model().setExpanded(synth, true);
    f.settle();
    CHECK(f.model().rows()[static_cast<std::size_t>(synth)].expanded);
    const int pad = f.rowNamed("Colundi Pad");
    REQUIRE(pad > synth);
    CHECK(f.model().rows()[static_cast<std::size_t>(pad)].depth == 1);
    // Left on a preset goes to its device; Left again closes it.
    f.model().select(pad);
    f.model().collapseOrParent(pad);
    CHECK(f.model().selectedRow() == synth);
    f.model().collapseOrParent(synth);
    CHECK(f.rowNamed("Colundi Pad") < 0);
    // A device with no presets loses its arrow once opened.
    const int sampler = f.rowNamed("Sampler");
    f.model().setExpanded(sampler, true);
    f.settle();
    CHECK_FALSE(f.model().rows()[static_cast<std::size_t>(f.rowNamed("Sampler"))].expandable);

    f.showSource(BrowserModel::Source::Kind::category, "Audio Effects");
    names = namesOf(f.model());
    CHECK(names.contains("Reverb"));
    CHECK(names.contains("Audio Effect Rack"));
    CHECK_FALSE(names.contains("Synth"));
    f.showSource(BrowserModel::Source::Kind::category, "MIDI Effects");
    CHECK(namesOf(f.model()).contains("Arpeggiator"));
    f.showSource(BrowserModel::Source::Kind::category, "Presets");
    CHECK(namesOf(f.model()).contains("Colundi Bass"));
    f.showSource(BrowserModel::Source::Kind::category, "Tunings");
    CHECK(namesOf(f.model()).contains("Colundi"));

    // A refresh (the library changed) keeps the tree open as it was and the selection on its row.
    f.showSource(BrowserModel::Source::Kind::category, "Instruments");
    f.model().setExpanded(f.rowNamed("Synth"), true);
    f.settle();
    f.model().select(f.rowNamed("Colundi Bass"));
    f.model().refresh();
    f.settle();
    REQUIRE(f.rowNamed("Colundi Bass") >= 0);
    CHECK(f.model().selectedRow() == f.rowNamed("Colundi Bass"));
}

TEST_CASE("Browser: a place's folders open, and search finds everything as you type", "[ui][browser]")
{
    Fixture f;
    TempFolder place("lattice-browser-place");
    REQUIRE(writeWav(place / "Kick 01.wav", 0.3));
    REQUIRE(writeWav(place / "Loops/Synth Loop.wav", 0.5, 44100.0, 2));
    REQUIRE(writeWav(place / "Loops/Deeper/Hat.wav", 0.1));
    f.shell->library().addPlace(place.folder);
    f.settle();

    f.showSource(BrowserModel::Source::Kind::place, place.folder.getFileName());
    CHECK(namesOf(f.model()) == juce::StringArray{"Loops", "Kick 01"}); // folders first
    f.model().setExpanded(0, true);
    f.settle();
    CHECK(namesOf(f.model()) == juce::StringArray{"Loops", "Deeper", "Synth Loop", "Kick 01"});
    f.model().setExpanded(1, true);
    f.settle();
    CHECK(f.model().rows()[2].item.name == "Hat");
    CHECK(f.model().rows()[2].depth == 2);
    // Closing a folder closes what's inside it too.
    f.model().setExpanded(0, false);
    CHECK(namesOf(f.model()) == juce::StringArray{"Loops", "Kick 01"});

    // Typing searches everything, ranked; clearing it goes back to the place.
    f.browser().searchBox().setText("synth", false);
    f.browser().searchBox().onTextChange();
    f.settle();
    CHECK(f.model().searching());
    auto names = namesOf(f.model());
    REQUIRE(names.size() >= 2);
    CHECK(names[0] == "Synth");
    CHECK(names.contains("Synth Loop"));
    CHECK(names.contains("Colundi Pad")); // a Synth preset
    f.browser().searchBox().setText("synth loop", false);
    f.browser().searchBox().onTextChange();
    f.settle();
    CHECK(namesOf(f.model()) == juce::StringArray{"Synth Loop"});
    f.browser().searchBox().setText({}, false);
    f.browser().searchBox().onTextChange();
    f.settle();
    CHECK(namesOf(f.model()) == juce::StringArray{"Loops", "Kick 01"});

    // Collections: from a row's menu (here, straight on the index), shown as a source.
    f.shell->library().index().setInCollection((place / "Kick 01.wav").getFullPathName(), 0, true);
    f.settle();
    f.showSource(BrowserModel::Source::Kind::collection, "Favourites");
    CHECK(namesOf(f.model()) == juce::StringArray{"Kick 01"});
    CHECK(f.model().rows()[0].item.inCollection(0));
}

TEST_CASE("Browser: Enter inserts where the Device view is looking, a rack's chain included", "[ui][browser]")
{
    // M3 hands-on finding 4: the debug UI always inserted into the track's top-level chain.
    Fixture f;
    f.shell->selection().set({SessionSelection::Kind::track, 0, 0});
    f.settle();
    auto& view = f.shell->deviceView();
    f.session.undoManager().beginNewTransaction("Rack");
    const auto rack = racks::insertRack(f.set(), f.chain(0), -1, RackDevice::instrumentRackId);
    racks::addChain(f.set(), rack);
    const auto second = racks::addChain(f.set(), rack);
    f.settle();
    view.focusChain(racks::devicesOf(second));

    f.showSource(BrowserModel::Source::Kind::category, "Instruments");
    f.model().select(f.rowNamed("Synth"));
    CHECK(f.browser().insertSelected());
    f.settle();
    REQUIRE(racks::devicesOf(second).getNumChildren() == 1);
    CHECK(racks::devicesOf(second).getChild(0)[ids::deviceType].toString() == "lattice.synth");
    CHECK(f.chain(0).getNumChildren() == 1); // the rack alone at the top
    CHECK(f.shell->detailView().page() == DetailView::Page::device);

    // A preset: the same place, as a copy of its device.
    f.model().setExpanded(f.rowNamed("Synth"), true);
    f.settle();
    f.answerYes = false;
    f.model().select(f.rowNamed("Colundi Bass"));
    CHECK_FALSE(f.browser().insertSelected()); // an instrument there already: asked first
    CHECK(f.questions.size() == 1);
    f.answerYes = true;
    f.browser().insertSelected();
    f.settle();
    REQUIRE(racks::devicesOf(second).getNumChildren() == 1);
    CHECK(racks::devicesOf(second).getChild(0)[ids::deviceType].toString() == "lattice.synth");

    // An effect after it; a sample makes a Sampler.
    f.showSource(BrowserModel::Source::Kind::category, "Audio Effects");
    f.model().select(f.rowNamed("Reverb"));
    f.browser().insertSelected();
    f.settle();
    CHECK(racks::devicesOf(second).getNumChildren() == 2);

    // A tuning goes to the selected track.
    f.showSource(BrowserModel::Source::Kind::category, "Tunings");
    f.model().select(f.rowNamed("Colundi"));
    CHECK(f.browser().insertSelected());
    CHECK(f.set().track(0)[ids::tuningId].toString() == TuningRegistry::colundiId);
    f.session.undoManager().undo();
    CHECK(f.set().track(0)[ids::tuningId].toString() != TuningRegistry::colundiId);
}

TEST_CASE("Browser: rows dropped on tracks, a device's title bar, a Sampler and a pad", "[ui][browser]")
{
    Fixture f;
    auto& sessionView = f.shell->sessionView();
    sessionView.flushUpdates();
    const auto headerAt = [&](int column) {
        const auto slot = sessionView.slotOnScreen(column, 0);
        return juce::Point<int>(slot.getCentreX(), sessionView.headerArea().getCentreY());
    };

    // Onto a track's column: into its chain, at the end; the track is selected and shown.
    REQUIRE(sessionView.isInterestedInDragSource(f.dragOf(drag::device("lattice.synth"), headerAt(1))));
    sessionView.itemDropped(f.dragOf(drag::device("lattice.synth"), headerAt(1)));
    f.settle();
    REQUIRE(f.chain(1).getNumChildren() == 1);
    CHECK(f.shell->selection().get().track == 1);
    // Anywhere in the column counts (a slot too).
    const auto slot = sessionView.slotOnScreen(1, 1).getCentre();
    sessionView.itemDropped(f.dragOf(drag::device("lattice.reverb"), slot));
    f.settle();
    CHECK(f.chain(1).getNumChildren() == 2);

    // A tuning onto a MIDI track's header sets it; onto a return, it's refused.
    sessionView.itemDropped(f.dragOf(drag::tuning(TuningRegistry::colundiId), headerAt(0)));
    CHECK(f.set().track(0)[ids::tuningId].toString() == TuningRegistry::colundiId);
    const auto& columns = sessionView.layout().columns();
    const auto returnColumn = static_cast<int>(std::find_if(columns.begin(), columns.end(), [](const auto& c) {
                                                   return c.kind == SessionLayout::Column::Kind::returnTrack;
                                               }) - columns.begin());
    REQUIRE(returnColumn < static_cast<int>(columns.size()));
    CHECK(sessionView.browserDropAt(headerAt(returnColumn), drag::tuning(TuningRegistry::colundiId)).area ==
          SessionView::Hit::Area::none);
    // A device onto a return goes into the return's chain.
    sessionView.itemDropped(f.dragOf(drag::device("lattice.delay"), headerAt(returnColumn)));
    f.settle();
    const auto returnChain = LiveSet::chainOf(f.set().returnTrack(columns[static_cast<std::size_t>(returnColumn)].index));
    CHECK(returnChain.getChild(returnChain.getNumChildren() - 1)[ids::deviceType].toString() == "lattice.delay");

    // A tuning file (from a place) loads, then sets the track's tuning.
    TempFolder folder("lattice-browser-tuning");
    REQUIRE((folder / "Five.scl").replaceWithText(scalaScale()));
    sessionView.itemDropped(f.dragOf(drag::tuning((folder / "Five.scl").getFullPathName()), headerAt(0)));
    const auto id = f.set().track(0)[ids::tuningId].toString();
    CHECK(id != TuningRegistry::colundiId);
    REQUIRE(f.tunings.findById(id) != nullptr);

    // Onto a device's title bar: it replaces the device (hot-swap); onto its body, the chain inserts.
    f.shell->selection().set({SessionSelection::Kind::track, 1, 0});
    f.settle();
    auto& view = f.shell->deviceView();
    const auto reverb = f.chain(1).getChild(1);
    auto* frame = view.frameFor(reverb);
    REQUIRE(frame != nullptr);
    const auto title = frame->titleBar().getCentre();
    const auto delay = f.dragOf(drag::device("lattice.delay"), title);
    CHECK(frame->dropReplaces(delay));
    CHECK_FALSE(frame->dropReplaces(f.dragOf(drag::device("lattice.delay"), {title.x, frame->getHeight() - 4})));
    frame->itemDropped(delay);
    f.settle();
    CHECK(f.chain(1).getNumChildren() == 2);
    CHECK(f.chain(1).getChild(1)[ids::deviceType].toString() == "lattice.delay");
    f.session.undoManager().undo();
    f.settle();
    CHECK(f.chain(1).getChild(1)[ids::deviceType].toString() == "lattice.reverb");

    // A sample onto a Sampler loads it; onto a pad, the pad gets it (the Device view's own tests
    // cover pads in full: the browser drags the same description).
    TempFolder samples("lattice-browser-samples");
    REQUIRE(writeWav(samples / "Tone A4.wav", 0.5, 48000.0, 1, 69));
    f.session.undoManager().beginNewTransaction("Sampler");
    const auto sampler = f.set().insertDevice(f.chain(0), -1, "lattice.sampler");
    f.shell->selection().set({SessionSelection::Kind::track, 0, 0});
    f.settle();
    auto* samplerFrame = view.frameFor(sampler);
    REQUIRE(samplerFrame != nullptr);
    const auto sampleDrag = f.dragOf(drag::sample(samples / "Tone A4.wav"), samplerFrame->getLocalBounds().getCentre());
    REQUIRE(samplerFrame->isInterestedInDragSource(sampleDrag));
    samplerFrame->itemDropped(sampleDrag);
    f.settle();
    CHECK(sampler.getChildWithName(ids::deviceState)["file"].toString() == (samples / "Tone A4.wav").getFullPathName());
}

TEST_CASE("Browser: selecting a sample previews it with its waveform; a preset plays a phrase", "[ui][browser][preview]")
{
    Fixture f;
    TempFolder place("lattice-browser-preview");
    REQUIRE(writeWav(place / "Tone.wav", 0.5));
    f.shell->library().addPlace(place.folder);
    f.settle();
    f.showSource(BrowserModel::Source::Kind::place, place.folder.getFileName());
    f.browser().setPreviewOn(true);
    auto& preview = f.engine.preview();
    const auto before = preview.soundsKept();

    f.model().select(f.rowNamed("Tone"));
    f.settle();
    CHECK(f.browser().showsWaveform());
    CHECK(f.shell->library().peaks().fileFor(place / "Tone.wav").existsAsFile()); // kept on disk
    // The sound reached the preview bus: it plays in the next block.
    f.engine.prepare(48000.0, 512);
    std::vector<float> left(512), right(512);
    float* outs[2] = {left.data(), right.data()};
    f.engine.process(outs, 2, 512);
    CHECK(preview.isPlaying());
    CHECK(*std::max_element(left.begin(), left.end()) > 0.05f);
    CHECK(preview.soundsKept() >= before);

    // Preview off: it stops, and selecting plays nothing.
    f.browser().setPreviewOn(false);
    f.engine.process(outs, 2, 512);
    f.engine.process(outs, 2, 512);
    CHECK_FALSE(preview.isPlaying());

    // An instrument preset plays its phrase.
    f.browser().setPreviewOn(true);
    f.showSource(BrowserModel::Source::Kind::category, "Presets");
    f.model().select(f.rowNamed("Colundi Pad"));
    for (int i = 0; i < 300 && !preview.isPlaying(); ++i)
    {
        f.settle();
        f.engine.process(outs, 2, 512);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(preview.isPlaying());
}

TEST_CASE("Browser: the commands", "[ui][browser]")
{
    Fixture f;
    auto& commands = f.shell->commands();
    f.shell->setBrowserVisible(false);
    REQUIRE(commands.perform("view.searchBrowser"));
    CHECK(f.shell->isBrowserVisible());
    CHECK(commands.shortcutsFor("view.searchBrowser").contains(
        juce::KeyPress('f', juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier, 0)));
    const bool on = f.browser().isPreviewOn();
    REQUIRE(commands.perform("view.preview"));
    CHECK(f.browser().isPreviewOn() != on);
    CHECK(commands.find("view.preview")->isTicked() == f.browser().isPreviewOn());
    REQUIRE(commands.perform("view.previewSync"));
    CHECK(f.browser().isPreviewSynced());
    CHECK(commands.conflicts().empty());
    // The preview settings are remembered.
    CHECK(f.settings->getBoolValue("browser.previewSync", false));
}

TEST_CASE("Browser timing: filling 100,000 search results paints within a frame", "[ui][browser][timing]")
{
    // Tests, item 10.
    Fixture f;
    f.browser().setSize(560, 900);
    std::vector<BrowserModel::Row> rows(100000);
    for (std::size_t i = 0; i < rows.size(); ++i)
    {
        auto& item = rows[i].item;
        item.kind = Kind::sample;
        item.key = "C:/Samples/Kit " + juce::String(static_cast<int>(i / 100)) + "/Hit " + juce::String(static_cast<int>(i)) + ".wav";
        item.name = "Hit " + juce::String(static_cast<int>(i));
        item.detail = library::sampleDetail(0.25 + static_cast<double>(i % 7), 48000.0, 1 + static_cast<int>(i % 2));
        item.collections = (i % 11 == 0) ? 1u : 0u;
    }
    juce::Image target(juce::Image::ARGB, f.browser().getWidth(), f.browser().getHeight(), true);
    std::vector<double> times;
    for (int frame = 0; frame < 30; ++frame)
    {
        auto copy = rows;
        const auto start = juce::Time::getHighResolutionTicks();
        f.model().setRowsForTests(std::move(copy));
        f.browser().scrollToRow(frame * 3000);
        juce::Graphics g(target);
        f.browser().paintEntireComponent(g, true);
        times.push_back(juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - start));
    }
    std::sort(times.begin(), times.end());
    const double median = times[times.size() / 2];
    WARN("Browser with 100,000 results: median frame " << median * 1000.0 << " ms, worst " << times.back() * 1000.0 << " ms");
    CHECK(median < 0.008);
}

TEST_CASE("Browser snapshot: in every theme at 100% and 200%", "[.ui-snapshot]")
{
    const auto dir = juce::SystemStats::getEnvironmentVariable("LATTICE_SNAPSHOT_DIR", {});
    if (dir.isEmpty())
        return;
    Fixture f;
    TempFolder place("lattice-browser-snap");
    REQUIRE(writeWav(place / "Kick 01.wav", 0.3));
    REQUIRE(writeWav(place / "Pad A4.wav", 1.25, 44100.0, 2, 69));
    REQUIRE(writeWav(place / "Loops/Synth Loop.wav", 2.0, 44100.0, 2));
    f.shell->library().addPlace(place.folder);
    f.settle();
    f.shell->library().index().setInCollection((place / "Kick 01.wav").getFullPathName(), 0, true);
    f.shell->library().index().setInCollection((place / "Kick 01.wav").getFullPathName(), 4, true);
    f.settle();
    f.showSource(BrowserModel::Source::Kind::category, "Instruments");
    f.model().setExpanded(f.rowNamed("Synth"), true);
    f.settle();
    f.model().select(f.rowNamed("Colundi Pad"));
    f.settle();
    for (const auto* name : {"Dark", "Light", "High Contrast"})
    {
        const auto* theme = Theme::builtIn(name);
        REQUIRE(theme != nullptr);
        f.shell->setTheme(*theme);
        for (const float scale : {1.0f, 2.0f})
        {
            for (const auto* page : {"instruments", "place"})
            {
                if (juce::String(page) == "place")
                {
                    f.showSource(BrowserModel::Source::Kind::place, place.folder.getFileName());
                    f.model().setExpanded(f.rowNamed("Loops"), true);
                    f.settle();
                    f.model().select(f.rowNamed("Pad A4"));
                    f.settle();
                }
                f.browser().setSize(560, 700);
                const auto image = f.browser().createComponentSnapshot(f.browser().getLocalBounds(), true, scale);
                const auto file = juce::File(dir).getChildFile("browser-" + juce::String(page) + "-" +
                                                               juce::String(name).replace(" ", "") + "-" +
                                                               juce::String(juce::roundToInt(scale * 100)) + ".png");
                file.deleteFile();
                juce::FileOutputStream out(file);
                juce::PNGImageFormat png;
                REQUIRE(png.writeImageToStream(image, out));
            }
            f.showSource(BrowserModel::Source::Kind::category, "Instruments");
        }
    }
}
