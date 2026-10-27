// The M4 4b hands-on checks (docs/05-m4-ui.md), run on the whole window's content as the app builds
// it: the shell with the Debug UI beside it, keys through the shell's key mappings and on to the
// computer keyboard, and the engine rendering offline. The bugs the hands-on checks found came
// from the two UIs together, which tests of either alone can't see.
//
// With LATTICE_SNAPSHOT_DIR set, each step also saves a picture of the window there
// (handson-*.png), for looking at: what a test can't judge is whether it reads on screen.

#include "app/MainWindow.h"
#include "app/debug/DeviceUi.h"
#include "devices/ChainEditing.h"
#include "devices/effects/Reverb.h"
#include "devices/racks/Rack.h"
#include "devices/racks/RackEditing.h"
#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace lattice;
using namespace lattice::test;

namespace
{
struct App
{
    App()
    {
        settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("lattice-handson", ".settings");
        juce::PropertiesFile::Options options;
        options.applicationName = "LatticeTests";
        settings = std::make_unique<juce::PropertiesFile>(settingsFile, options);
        session.newSet(4, 8); // as File > New: track 1 armed
        for (int t = 0; t < 4; ++t)
        {
            auto chain = LiveSet::chainOf(set().track(t));
            while (chain.getNumChildren() > 0)
                set().removeDevice(chain.getChild(0));
            set().insertDevice(chain, -1, TestRecorderDevice::typeId); // sounds while a note is held
        }
        session.flush();
        window = std::make_unique<MainComponent>(engine, *settings, tunings, session, nullptr);
        window->setSize(1600, 1000);
        settle();
    }
    ~App()
    {
        window.reset();
        settings.reset();
        settingsFile.deleteFile();
    }

    LiveSet& set() { return session.set(); }
    ui::MainShell& shell() { return window->shell(); }
    ui::SessionView& view() { return shell().sessionView(); }
    ComputerKeyboardInput& keyboard() { return window->debugUi().playPanel().computerKeyboard(); }
    /** What the message loop would do between events: document updates, the debug UI's timer. */
    void settle()
    {
        view().flushUpdates();
        window->debugUi().sessionView().flushForTests();
        window->debugUi().sessionView().detailView().flushUpdates();
        shell().clipView().flushUpdates();
    }
    bool key(const juce::KeyPress& k)
    {
        settle();
        // As a key event goes: the shell's commands first, then the window's (the computer keyboard).
        const bool taken = shell().commands().keyMappings().keyPressed(k, &shell()) || window->keyPressed(k);
        settle();
        return taken;
    }
    bool ctrl(char c) { return key(juce::KeyPress(c, juce::ModifierKeys::commandModifier, 0)); }
    bool ctrlAlt(char c) { return key(juce::KeyPress(c, juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier, 0)); }
    void clickHeader(int column, juce::ModifierKeys mods = {})
    {
        settle();
        const auto& c = view().layout().columns()[static_cast<std::size_t>(column)];
        const auto slot = view().slotOnScreen(column, 0);
        view().press(view().hitAt({slot.getX() + c.width / 4, view().headerArea().getCentreY()}), mods);
        view().release();
        settle();
    }
    /** A frame of the UI: the state feed's tick (the meters take the peaks, as on screen). */
    void frame()
    {
        settle();
        now += 1.0 / 60.0;
        shell().feed().tick(now);
        settle();
    }
    /** Plays the computer keyboard's A for a moment; returns each track's peak meanwhile, as its
        meter got it. With a name, takes a snapshot while the note sounds. */
    std::vector<float> playA(const char* snapshot = nullptr)
    {
        render(480);
        frame();
        key(juce::KeyPress('a', 0, 'a'));
        render(4800);
        frame();
        std::vector<float> peaks;
        for (int t = 0; t < set().numTracks(); ++t)
            peaks.push_back(shell().feed().trackPeaks(t)[0]);
        if (snapshot != nullptr)
            snap(snapshot);
        keyboard().releaseAll();
        render(9600);
        frame();
        return peaks;
    }
    /** Saves a picture of the window, if LATTICE_SNAPSHOT_DIR is set. */
    void snap(const juce::String& name, float scale = 1.0f)
    {
        const auto dir = juce::SystemStats::getEnvironmentVariable("LATTICE_SNAPSHOT_DIR", {});
        if (dir.isEmpty())
            return;
        settle();
        const auto image = window->createComponentSnapshot(window->getLocalBounds(), true, scale);
        const auto file = juce::File(dir).getChildFile("handson-" + name + ".png");
        file.deleteFile();
        juce::FileOutputStream out(file);
        juce::PNGImageFormat png;
        REQUIRE(png.writeImageToStream(image, out));
    }
    void render(std::int64_t samples)
    {
        settle();
        session.flush();
        renderer.renderFor(samples);
        session.processEngineEvents();
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer{engine, BlockPattern::Fixed64};
    juce::File settingsFile;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<MainComponent> window;
    double now = 10.0;
};
} // namespace

TEST_CASE("Hands-on 4b, 3: the mixer", "[app][handson]")
{
    App app;
    auto& view = app.view();

    SECTION("1: the mixer stays pinned while the grid scrolls")
    {
        const auto mixer = view.mixerArea();
        REQUIRE(!mixer.isEmpty());
        CHECK(mixer.getBottom() <= view.getHeight());
        CHECK(mixer.getBottom() >= view.getHeight() - 20); // at the bottom (above the sideways scroll bar)
        for (int i = 0; i < 12; ++i)
            REQUIRE(app.ctrl('i'));
        CHECK(app.set().numScenes() == 20);
        app.snap("3-1-scenes-added");
        view.setScrollPosition({0, 100000});
        CHECK(view.scrollPosition().y > 0); // it scrolled
        app.snap("3-1-scrolled-to-the-bottom");
        CHECK(view.mixerArea() == mixer);
        CHECK(view.slotsArea().getBottom() <= mixer.getY());
        CHECK(view.stripForColumn(0)->isVisible());
    }

    SECTION("2: the fader, the dB box and the meter")
    {
        auto* strip = view.stripForColumn(0);
        strip->fader()->beginDrag();
        strip->fader()->dragBy(-40.0f, false);
        strip->fader()->endDrag();
        CHECK(static_cast<double>(app.set().track(0)[ids::volumeDb]) < 0.0);
        strip->volumeBox()->commitTypeIn("-6");
        CHECK(static_cast<double>(app.set().track(0)[ids::volumeDb]) == Catch::Approx(-6.0));
        // Track 1 is armed (as in a new set): A plays it, and its meter moves.
        app.render(480);
        app.key(juce::KeyPress('a', 0, 'a'));
        app.render(4800);
        app.frame();
        CHECK(strip->fader()->meter().peakDb(0) > -60.0f);
        app.snap("3-2-fader-at-minus-6-meter-moving");
        app.keyboard().releaseAll();
    }

    SECTION("3: a send reads in dB")
    {
        auto& send = *view.stripForColumn(0)->sendKnobs()[0];
        send.commitTypeIn("-12");
        CHECK(send.binding().text().endsWith(" dB"));
        CHECK(send.binding().text().startsWith("-12"));
        CHECK(app.set().sendLevel(0, 0) == Catch::Approx(-12.0));
        app.snap("3-3-send-A-at-minus-12");
    }

    SECTION("4: input and monitoring")
    {
        auto* strip = view.stripForColumn(0);
        strip->inputBox()->binding().setValue(1.0f); // Computer Keyboard
        CHECK(app.set().track(0)[ids::midiInput].toString() == LiveSet::computerKeyboardInput);
        CHECK(app.playA("3-4-computer-keyboard-auto-playing")[0] > 0.0f); // armed, Auto: plays

        strip->monitorButton(Monitoring::Off)->binding()->setValue(1.0f);
        CHECK(app.playA("3-4-monitoring-off-silent")[0] == 0.0f); // Off: silence

        strip->monitorButton(Monitoring::In)->binding()->setValue(1.0f);
        app.set().setTrackArm(0, false, false);
        app.clickHeader(1); // auto-arm takes track 2, so track 1 is surely disarmed
        CHECK(!app.session.isArmed(0));
        CHECK(app.playA("3-4-monitoring-in-disarmed-playing")[0] > 0.0f); // In: plays, disarmed
    }

    SECTION("5: showing and hiding the mixer, its I/O and its sends")
    {
        CHECK(app.ctrlAlt('m'));
        CHECK(!view.isMixerVisible());
        app.snap("3-5-mixer-hidden");
        CHECK(app.ctrlAlt('m'));
        CHECK(view.isMixerVisible());
        CHECK(app.ctrlAlt('i'));
        CHECK(!view.isIOVisible());
        CHECK(!view.stripForColumn(0)->inputBox()->isVisible());
        app.snap("3-5-io-hidden");
        CHECK(app.ctrlAlt('i'));
        CHECK(view.stripForColumn(0)->inputBox()->isVisible());
        CHECK(app.ctrlAlt('s'));
        CHECK(!view.areSendsVisible());
        CHECK(!view.stripForColumn(0)->sendKnobs()[0]->isVisible());
        app.snap("3-5-sends-hidden");
        CHECK(app.ctrlAlt('s'));
        CHECK(view.stripForColumn(0)->sendKnobs()[0]->isVisible());
    }

    SECTION("6: the tuning badge")
    {
        const auto badge = view.tuningBadge(0);
        REQUIRE(!badge.isEmpty());
        CHECK(view.hitAt(badge.getCentre()).area == ui::SessionView::Hit::Area::tuning);
        auto menu = view.tuningMenu(0);
        juce::PopupMenu::MenuItemIterator it(menu);
        juce::String picked;
        while (it.next())
            if (!it.getItem().isTicked && picked.isEmpty())
            {
                picked = it.getItem().text;
                it.getItem().action();
            }
        REQUIRE(picked.isNotEmpty());
        const auto tuning = app.tunings.findById(app.set().track(0)[ids::tuningId].toString());
        REQUIRE(tuning != nullptr);
        CHECK(tuning->name == picked);
        app.snap("3-6-track-1-tuning-changed");
    }
}

TEST_CASE("Hands-on 4b, 4: auto-arm and the computer keyboard", "[app][handson]")
{
    App app;
    auto* strip1 = app.view().stripForColumn(0);
    strip1->armButton()->binding()->setValue(0.0f); // 1: disarm track 1 (monitoring is Auto)
    CHECK(!static_cast<bool>(app.set().track(0)[ids::arm]));
    CHECK(app.shell().commands().find("session.autoArm")->isTicked()); // 2

    // 3: track 1 selected: A plays it, and it shows as auto-armed (the ring).
    app.clickHeader(0);
    CHECK(app.session.isAutoArmed(0));
    auto peaks = app.playA("4-3-track-1-auto-armed-playing");
    CHECK(peaks[0] > 0.0f);
    CHECK(peaks[1] == 0.0f);

    // 4: track 2 selected: A plays it instead.
    app.clickHeader(1);
    CHECK(app.session.isAutoArmed(1));
    CHECK(!app.session.isArmed(0));
    peaks = app.playA("4-4-track-2-auto-armed-playing");
    CHECK(peaks[0] == 0.0f);
    CHECK(peaks[1] > 0.0f);

    // 5: a Drum Rack on track 3: A plays its first pad there, and the range comes back after.
    const int base = app.keyboard().getBaseNote();
    auto chain = LiveSet::chainOf(app.set().track(2));
    while (chain.getNumChildren() > 0)
        app.set().removeDevice(chain.getChild(0));
    auto rack = chains::insert(app.set(), chain, RackDevice::drumRackId, -1, testRegistry());
    REQUIRE(rack.isValid());
    racks::addDrumPad(app.set(), rack, 36);
    racks::addDrumPad(app.set(), rack, 38);
    app.clickHeader(2);
    CHECK(app.keyboard().getBaseNote() == 36);
    app.snap("4-5-drum-rack-track-selected");
    app.clickHeader(1);
    CHECK(app.keyboard().getBaseNote() == base);
}

TEST_CASE("Hands-on 4b, 5: an effect inserted on a return starts fully wet", "[app][handson]")
{
    App app;
    auto& debug = app.window->debugUi().sessionView();
    // As in the Debug UI tab: show Return A's chain and insert a Reverb from the device list.
    debug.showChainOf({debugui::ChainOwner::Kind::Return, 0});
    REQUIRE(debug.detailView().chainRow() != nullptr);
    debug.detailView().chainRow()->insertFromDescription(debugui::drag::device(ReverbDevice::typeId), -1);
    auto returnChain = LiveSet::chainOf(app.set().returnTrack(0));
    REQUIRE(returnChain.getNumChildren() == 1);
    CHECK(LiveSet::deviceParameter(returnChain.getChild(0), "reverb.mix", 0.0) == 1.0);
    app.shell().mainArea().setCurrentTabIndex(2); // the Debug UI tab
    app.snap("5-1-reverb-on-return-A");

    // On a track it keeps its usual default.
    debug.showChainOf({debugui::ChainOwner::Kind::Track, 0});
    debug.detailView().chainRow()->insertFromDescription(debugui::drag::device(ReverbDevice::typeId), -1);
    auto trackChain = LiveSet::chainOf(app.set().track(0));
    REQUIRE(trackChain.getNumChildren() == 2);
    CHECK(LiveSet::deviceParameter(trackChain.getChild(1), "reverb.mix", -1.0) == -1.0); // the default
}

TEST_CASE("Hands-on 4b, 2: groups, on screen", "[app][handson]")
{
    App app;
    auto& set = app.set();
    for (int t = 1; t < 3; ++t)
        set.addNote(set.createClip(t, 0, 4.0), {60, 0.0, 4.0, 100, false});
    app.clickHeader(1);
    app.clickHeader(2, juce::ModifierKeys::shiftModifier);
    REQUIRE(app.ctrl('g')); // G between 1 and 2: 1 G >2 >3 4
    REQUIRE(set.isGroupTrack(1));
    CHECK(set.groupMembers(1).size() == 2);
    app.session.play();
    app.view().press(app.view().hitAt(app.view().layout().launchButton(app.view().slotOnScreen(1, 0)).getCentre()));
    app.view().release();
    app.render(48000 * 2);
    app.frame();
    app.snap("2-group-of-two-playing");

    app.view().press(app.view().hitAt(app.view().layout().foldButton(1)
                                          .translated(app.view().slotsArea().getX() - app.view().scrollPosition().x,
                                                      app.view().headerArea().getY())
                                          .getCentre()));
    app.view().release();
    app.settle();
    CHECK(static_cast<bool>(set.track(1)[ids::folded]));
    app.snap("2-group-folded");
}

TEST_CASE("Hands-on 4b: the window in every theme, at 200%", "[app][handson]")
{
    App app;
    auto& set = app.set();
    set.addNote(set.createClip(0, 0, 4.0), {60, 0.0, 4.0, 100, false});
    set.groupTracks(2, 3);
    app.session.play();
    app.session.launchSlot(0, 0);
    app.render(48000);
    app.frame();
    for (const auto* theme : {&ui::Theme::dark(), &ui::Theme::light(), &ui::Theme::highContrast()})
    {
        app.shell().setTheme(*theme);
        app.render(2400); // the meters show what played since the last frame
        app.frame();
        app.snap("theme-" + theme->name.removeCharacters(" ").toLowerCase() + "-200", 2.0f);
    }
}

TEST_CASE("Hands-on 4c: double-clicking a clip opens it in the Clip view and Shift+Tab switches pages", "[app][handson]")
{
    App app;
    auto& set = app.set();
    set.setTrackTuning(0, TuningRegistry::colundiId);
    const auto clip = set.createClip(0, 1, 4.0, "Riff");
    for (int i = 0; i < 4; ++i)
        set.addNote(clip, {40 + i * 3, i * 1.0, 0.5, 100, false});
    app.settle();
    REQUIRE(!app.shell().isDetailVisible());

    auto& view = app.view();
    view.doubleClick(view.hitAt(view.slotOnScreen(0, 1).getCentre()));
    app.settle();
    CHECK(app.shell().isDetailVisible());
    CHECK(app.shell().detailView().page() == ui::DetailView::Page::clip);
    CHECK(app.shell().clipView().clip() == clip);
    CHECK(app.shell().clipView().pianoRoll().notes().size() == 4);
    app.snap("4c-clip-view-colundi");

    // Shift+Tab: the Device view, and back.
    CHECK(app.key(juce::KeyPress(juce::KeyPress::tabKey, juce::ModifierKeys::shiftModifier, 0)));
    CHECK(app.shell().detailView().page() == ui::DetailView::Page::device);
    CHECK(app.key(juce::KeyPress(juce::KeyPress::tabKey, juce::ModifierKeys::shiftModifier, 0)));
    CHECK(app.shell().detailView().page() == ui::DetailView::Page::clip);

    // Double-clicking an empty slot makes a clip and shows it.
    view.doubleClick(view.hitAt(view.slotOnScreen(1, 2).getCentre()));
    app.settle();
    CHECK(set.clip(1, 2).isValid());
    CHECK(app.shell().clipView().clip() == set.clip(1, 2));
}
