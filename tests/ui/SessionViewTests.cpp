// The Session view (docs/05-m4-ui.md, 4b), headless: its layout (folded groups hide their tracks,
// returns follow the tracks), what a click hits, what clicks do (launching reaches the engine, a
// Gate clip's release too), scrolling, and a snapshot.

#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"
#include "ui/commands/SessionCommands.h"
#include "ui/session/SessionView.h"
#include "ui/theme/LatticeLookAndFeel.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace lattice;
using namespace lattice::ui;
using namespace lattice::test;
using Area = SessionView::Hit::Area;
using Kind = SessionSelection::Kind;

namespace
{
struct Fixture
{
    explicit Fixture(int tracks = 4, int scenes = 4)
    {
        session.newSet(tracks, scenes);
        session.flush();
        addSessionCommands(commands, session, {[this] { return selection.get(); }, [this](SessionSelection s) { selection.set(s); }}, {});
        view = std::make_unique<SessionView>(session, feed, commands, selection);
        view->setLookAndFeel(&laf);
        view->setSize(1000, 800);
    }
    ~Fixture() { view->setLookAndFeel(nullptr); }

    void render(std::int64_t samples)
    {
        session.flush();
        renderer.renderFor(samples);
        session.processEngineEvents();
        now += 1.0 / 60.0;
        feed.tick(now);
    }
    /** The centre of a slot's launch button, or its body, on screen. */
    juce::Point<int> launchOf(int column, int scene) const
    {
        return view->layout().launchButton(view->slotOnScreen(column, scene)).getCentre();
    }
    juce::Point<int> bodyOf(int column, int scene) const
    {
        const auto r = view->slotOnScreen(column, scene);
        return {r.getRight() - 4, r.getCentreY()};
    }
    void rebuildNow() { view->setZoom(1.0f, 1.0f); } // what the async rebuild does

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer{engine, BlockPattern::Fixed64};
    EngineStateFeed feed{session, engine};
    CommandRegistry commands{&session.undoManager()};
    SessionSelectionModel selection;
    LatticeLookAndFeel laf;
    std::unique_ptr<SessionView> view;
    double now = 3.0;
};
} // namespace

TEST_CASE("Session view: a column per visible track, then the returns; a folded group hides its tracks",
          "[ui][session]")
{
    Fixture f(4);
    auto& set = f.session.set();
    const auto returns = set.numReturns();
    f.rebuildNow();
    REQUIRE(f.view->layout().columns().size() == static_cast<std::size_t>(4 + returns));
    CHECK(f.view->layout().columns()[4].kind == SessionLayout::Column::Kind::returnTrack);

    set.groupTracks(1, 2); // A G >B >C D
    f.rebuildNow();
    CHECK(f.view->layout().columns().size() == static_cast<std::size_t>(5 + returns));
    CHECK(f.view->layout().columns()[1].isGroup);
    CHECK(f.view->layout().columns()[2].depth == 1);

    set.setGroupFolded(1, true);
    f.rebuildNow();
    CHECK(f.view->layout().columns().size() == static_cast<std::size_t>(3 + returns));
    CHECK(f.view->layout().columnOfTrack(2) == -1); // hidden
    CHECK(f.view->layout().columnOfTrack(4) == 2);

    // Zoom widens the columns and the rows.
    const int w = f.view->layout().columns()[0].width, h = f.view->layout().rowHeight();
    f.view->setZoom(2.0f, 1.5f);
    CHECK(f.view->layout().columns()[0].width == 2 * w);
    CHECK(f.view->layout().rowHeight() > h);
}

TEST_CASE("Session view: clicks hit headers, launch buttons, slots, stop buttons and scenes", "[ui][session]")
{
    Fixture f;
    const auto header = f.view->headerArea();
    const auto stops = f.view->stopArea();
    const auto scenes = f.view->sceneColumn();
    CHECK(f.view->hitAt({header.getX() + 30, header.getCentreY()}).area == Area::header);
    CHECK(f.view->hitAt(f.launchOf(1, 2)).area == Area::launch);
    const auto body = f.view->hitAt(f.bodyOf(1, 2));
    CHECK(body.area == Area::slot);
    CHECK(body.column == 1);
    CHECK(body.scene == 2);
    CHECK(f.view->hitAt({f.view->slotOnScreen(2, 0).getCentreX(), stops.getCentreY()}).area == Area::stop);
    const int sceneRow = f.view->slotOnScreen(0, 3).getCentreY();
    CHECK(f.view->hitAt({scenes.getX() + 5, sceneRow}).area == Area::sceneLaunch);
    CHECK(f.view->hitAt({scenes.getRight() - 5, sceneRow}).area == Area::scene);
    CHECK(f.view->hitAt({scenes.getCentreX(), stops.getCentreY()}).area == Area::stopAll);
}

TEST_CASE("Session view: a launch button launches (a Gate clip until released); the body selects",
          "[ui][session]")
{
    Fixture f;
    auto& set = f.session.set();
    auto clip = set.createClip(1, 0, 4.0);
    clip.setProperty(ids::launchMode, static_cast<int>(LaunchMode::Gate), nullptr);
    f.render(480);

    f.view->press(f.view->hitAt(f.bodyOf(1, 0)));
    CHECK(f.selection.get() == SessionSelection{Kind::slot, 1, 0});
    f.view->release();
    f.render(48000 * 3);
    CHECK(f.session.slotState(1, 0) == SlotState::Stopped); // selecting doesn't launch

    f.view->press(f.view->hitAt(f.launchOf(1, 0)));
    f.render(48000 * 3);
    CHECK(f.session.slotState(1, 0) == SlotState::Playing);
    f.view->release(); // a Gate clip stops when its button is let go
    f.render(4800);
    CHECK(f.session.slotState(1, 0) != SlotState::Playing);
}

TEST_CASE("Session view: double-clicks create clips and rename; the fold button folds; Stop All stops",
          "[ui][session]")
{
    Fixture f;
    auto& set = f.session.set();
    f.view->doubleClick(f.view->hitAt(f.bodyOf(2, 1)));
    CHECK(set.clip(2, 1).isValid());
    CHECK(f.selection.get() == SessionSelection{Kind::slot, 2, 1});
    f.session.undoManager().undo();
    CHECK(!set.clip(2, 1).isValid());

    set.groupTracks(0, 1);
    f.rebuildNow();
    const auto fold = f.view->layout().foldButton(0).translated(f.view->slotsArea().getX(), f.view->headerArea().getY()).getCentre();
    REQUIRE(f.view->hitAt(fold).area == Area::fold);
    f.view->press(f.view->hitAt(fold));
    CHECK(static_cast<bool>(set.track(0)[ids::folded]));
    f.rebuildNow();
    CHECK(f.view->layout().columnOfTrack(1) == -1);

    set.createClip(4, 0, 4.0);
    f.session.launchSlot(4, 0);
    f.render(48000 * 3);
    REQUIRE(f.session.slotState(4, 0) == SlotState::Playing);
    const auto stopAll = juce::Point<int>(f.view->sceneColumn().getCentreX(), f.view->stopArea().getCentreY());
    f.view->press(f.view->hitAt(stopAll));
    f.render(48000 * 3);
    CHECK(f.session.slotState(4, 0) == SlotState::Stopped);
}

TEST_CASE("Session view: the selection stays in view as it moves", "[ui][session]")
{
    Fixture f(20, 40);
    f.rebuildNow();
    CHECK(f.view->scrollPosition() == juce::Point<int>());
    f.selection.set({Kind::slot, 19, 39});
    const auto r = f.view->slotOnScreen(f.view->layout().columnOfTrack(19), 39);
    CHECK(f.view->slotsArea().contains(r));
    CHECK(f.view->scrollPosition().x > 0);
    CHECK(f.view->scrollPosition().y > 0);
    f.view->setScrollPosition({-50, 1000000}); // clamped
    CHECK(f.view->scrollPosition().x == 0);
    CHECK(f.view->scrollPosition().y == f.view->layout().rowsHeight() - f.view->slotsArea().getHeight());
}

TEST_CASE("Session view: a snapshot, for looking at", "[.ui-snapshot]")
{
    Fixture f(6, 6);
    auto& set = f.session.set();
    for (const auto& [t, s] : {std::pair{0, 0}, {0, 1}, {1, 0}, {3, 2}, {4, 0}})
    {
        auto c = set.createClip(t, s, 4.0);
        c.setProperty(ids::name, "Clip " + juce::String(t + 1) + "-" + juce::String(s + 1), nullptr);
    }
    set.groupTracks(3, 4);
    set.setTrackArm(2, true, false);
    set.setSceneName(0, "Intro");
    set.setSceneTempo(1, 98.0);
    f.rebuildNow();
    f.session.launchSlot(0, 0);
    f.session.launchSlot(1, 0);
    f.render(48000 * 3);
    f.session.launchSlot(0, 1);
    f.render(4800);
    f.selection.set({Kind::slot, 1, 0});
    const auto dir = juce::File(juce::SystemStats::getEnvironmentVariable(
        "LATTICE_SNAPSHOT_DIR", juce::File::getSpecialLocation(juce::File::tempDirectory).getFullPathName()));
    const auto image = f.view->createComponentSnapshot(f.view->getLocalBounds(), true, 1.0f);
    juce::PNGImageFormat png;
    const auto file = dir.getChildFile("session-view.png");
    file.deleteFile();
    juce::FileOutputStream out(file);
    REQUIRE(png.writeImageToStream(image, out));
    WARN("Wrote " << file.getFullPathName());
}

TEST_CASE("Session view: a mixer strip under every column and the master's under the scenes", "[ui][session][mixer]")
{
    Fixture f(3);
    auto& set = f.session.set();
    set.groupTracks(0, 1);
    f.rebuildNow();
    const auto& columns = f.view->layout().columns();
    for (int c = 0; c < static_cast<int>(columns.size()); ++c)
    {
        auto* strip = f.view->stripForColumn(c);
        REQUIRE(strip != nullptr);
        CHECK(strip->index() == columns[static_cast<std::size_t>(c)].index);
        const bool isTrack = columns[static_cast<std::size_t>(c)].kind == SessionLayout::Column::Kind::track;
        CHECK((strip->kind() == MixerStrip::Kind::track) == isTrack);
        if (isTrack)
        {
            CHECK(static_cast<int>(strip->sendKnobs().size()) == set.numReturns());
            CHECK((strip->armButton() == nullptr) == set.isGroupTrack(strip->index())); // no arm on a group
        }
        else
            CHECK(strip->preFaderButton() != nullptr);
    }
    REQUIRE(f.view->masterStrip() != nullptr);
    CHECK(f.view->masterStrip()->getX() >= f.view->sceneColumn().getX());

    // Strips sit under their columns, and scroll with them.
    auto* first = f.view->stripForColumn(1);
    const int x = first->getScreenX();
    f.view->setScrollPosition({40, 0});
    if (f.view->scrollPosition().x > 0)
        CHECK(first->getScreenX() == x - f.view->scrollPosition().x);
}

TEST_CASE("Session view: the mixer's controls write the set; a drag doesn't rebuild the strips", "[ui][session][mixer]")
{
    Fixture f(2);
    auto& set = f.session.set();
    f.rebuildNow();
    auto* strip = f.view->stripForColumn(0);
    REQUIRE(strip != nullptr);

    strip->fader()->beginDrag();
    strip->fader()->dragBy(-30.0f, false);
    strip->fader()->endDrag();
    const double db = set.track(0)[ids::volumeDb];
    CHECK(db < 0.0);
    CHECK(strip->volumeBox()->binding().value() == Catch::Approx(static_cast<float>(db)));
    f.view->flushUpdates();
    CHECK(f.view->stripForColumn(0) == strip); // a property change only repaints

    strip->volumeBox()->commitTypeIn("-inf");
    CHECK(static_cast<double>(set.track(0)[ids::volumeDb]) == LiveSet::minVolumeDb);
    CHECK(strip->volumeBox()->binding().text() == "-inf dB");

    // The activator is lit while the track is on.
    CHECK(strip->activator()->isOn());
    strip->activator()->binding()->setValue(0.0f);
    CHECK(static_cast<bool>(set.track(0)[ids::mute]));
    CHECK(!strip->activator()->isOn());

    // A send shows its level in dB, and writes it.
    REQUIRE(!strip->sendKnobs().empty());
    auto& send = *strip->sendKnobs()[0];
    CHECK(send.binding().text() == "-inf dB");
    send.commitTypeIn("-12");
    CHECK(set.sendLevel(0, 0) == Catch::Approx(-12.0));
    CHECK(send.getTitle() == "Send to " + set.returnTrack(0)[ids::name].toString());

    // A new track does rebuild them.
    set.addTrack();
    f.view->flushUpdates();
    CHECK(f.view->stripForColumn(2) != nullptr);
    CHECK(f.view->stripForColumn(2)->kind() == MixerStrip::Kind::track);
}

TEST_CASE("Session view: the mixer stays in view however many scenes; it and its sends can be hidden",
          "[ui][session][mixer]")
{
    Fixture f(4, 64);
    f.rebuildNow();
    auto* strip = f.view->stripForColumn(0);
    REQUIRE(strip != nullptr);
    REQUIRE(strip->armButton() != nullptr);
    const auto arm = strip->armButton()->getScreenBounds();
    CHECK(f.view->getScreenBounds().contains(arm)); // with 64 scenes, the arm button still shows
    f.selection.set({Kind::slot, 0, 63});
    CHECK(f.view->getScreenBounds().contains(strip->armButton()->getScreenBounds()));

    const int mixerHeight = f.view->mixerArea().getHeight();
    f.view->setSendsVisible(false);
    CHECK(f.view->mixerArea().getHeight() < mixerHeight);
    CHECK(!strip->sendKnobs()[0]->isVisible());
    f.view->setMixerVisible(false);
    CHECK(f.view->mixerArea().getHeight() == 0);
    CHECK(f.view->stripForColumn(0) == nullptr);
}

TEST_CASE("Session view: meters follow the engine", "[ui][session][mixer]")
{
    Fixture f(2);
    auto& set = f.session.set();
    auto chain = LiveSet::chainOf(set.track(0));
    while (chain.getNumChildren() > 0)
        set.removeDevice(chain.getChild(0));
    set.insertDevice(chain, -1, TestRecorderDevice::typeId);
    set.setTrackArm(0, true, false);
    f.rebuildNow();
    f.render(480);
    f.engine.midiInput().push(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)));
    f.render(4800);
    CHECK(f.view->stripForColumn(0)->fader()->meter().peakDb(0) > -60.0f);
    CHECK(f.view->stripForColumn(1)->fader()->meter().peakDb(0) == Meter::floorDb);
    CHECK(f.view->masterStrip()->fader()->meter().peakDb(0) > -60.0f);
}

TEST_CASE("Session view: a track's I/O sets its MIDI input, channel and monitoring", "[ui][session][mixer]")
{
    Fixture f(2);
    auto& set = f.session.set();
    set.groupTracks(1, 1); // A G >B
    f.rebuildNow();
    auto* strip = f.view->stripForColumn(0);
    REQUIRE(strip->inputBox() != nullptr);
    CHECK(f.view->stripForColumn(1)->inputBox() == nullptr); // a group has no input

    auto& in = strip->inputBox()->binding();
    CHECK(in.text() == "All Ins");
    strip->inputBox()->commitTypeIn("Computer Keyboard");
    CHECK(set.track(0)[ids::midiInput].toString() == LiveSet::computerKeyboardInput);
    f.session.undoManager().undo();
    CHECK(set.track(0)[ids::midiInput].toString().isEmpty());

    strip->channelBox()->commitTypeIn("Ch. 3");
    CHECK(static_cast<int>(set.track(0)[ids::midiChannel]) == 3);

    // Monitoring: one lit at a time; clicking the lit one goes back to Auto.
    CHECK(strip->monitorButton(Monitoring::Auto)->isOn());
    strip->monitorButton(Monitoring::In)->binding()->setValue(1.0f);
    CHECK(static_cast<int>(set.track(0)[ids::monitoring]) == static_cast<int>(Monitoring::In));
    CHECK(strip->monitorButton(Monitoring::In)->isOn());
    CHECK(!strip->monitorButton(Monitoring::Auto)->isOn());
    strip->monitorButton(Monitoring::In)->binding()->setValue(0.0f);
    CHECK(static_cast<int>(set.track(0)[ids::monitoring]) == static_cast<int>(Monitoring::Auto));

    // An input the track names but that isn't connected still shows.
    set.setTrackMidiInput(0, "gone-device");
    f.rebuildNow();
    CHECK(f.view->stripForColumn(0)->inputBox()->binding().text() == "gone-device (not connected)");

    const int height = f.view->mixerArea().getHeight();
    f.view->setIOVisible(false);
    CHECK(f.view->mixerArea().getHeight() < height);
    CHECK(!f.view->stripForColumn(0)->inputBox()->isVisible());
}
