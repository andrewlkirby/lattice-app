// The M4 4b hands-on checks (docs/05-m4-ui.md), run on the whole window's content as the app builds
// it: the shell with the Debug UI beside it, keys through the shell's key mappings and on to the
// computer keyboard, and the engine rendering offline. The bugs the hands-on checks found came
// from the two UIs together, which tests of either alone can't see.

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
    /** Plays the computer keyboard's A for a moment; returns each track's peak meanwhile. */
    std::vector<float> playA()
    {
        render(480);
        for (int t = 0; t < set().numTracks(); ++t)
            session.takeTrackPeaks(t);
        key(juce::KeyPress('a', 0, 'a'));
        render(4800);
        std::vector<float> peaks;
        for (int t = 0; t < set().numTracks(); ++t)
            peaks.push_back(session.takeTrackPeaks(t)[0]);
        keyboard().releaseAll();
        render(9600);
        return peaks;
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
        view.setScrollPosition({0, 100000});
        CHECK(view.scrollPosition().y > 0); // it scrolled
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
        app.shell().feed().tick(10.0);
        CHECK(strip->fader()->meter().peakDb(0) > -60.0f);
        app.keyboard().releaseAll();
    }

    SECTION("3: a send reads in dB")
    {
        auto& send = *view.stripForColumn(0)->sendKnobs()[0];
        send.commitTypeIn("-12");
        CHECK(send.binding().text().endsWith(" dB"));
        CHECK(send.binding().text().startsWith("-12"));
        CHECK(app.set().sendLevel(0, 0) == Catch::Approx(-12.0));
    }

    SECTION("4: input and monitoring")
    {
        auto* strip = view.stripForColumn(0);
        strip->inputBox()->binding().setValue(1.0f); // Computer Keyboard
        CHECK(app.set().track(0)[ids::midiInput].toString() == LiveSet::computerKeyboardInput);
        CHECK(app.playA()[0] > 0.0f); // armed, Auto: plays

        strip->monitorButton(Monitoring::Off)->binding()->setValue(1.0f);
        CHECK(app.playA()[0] == 0.0f); // Off: silence

        strip->monitorButton(Monitoring::In)->binding()->setValue(1.0f);
        app.set().setTrackArm(0, false, false);
        app.clickHeader(1); // auto-arm takes track 2, so track 1 is surely disarmed
        CHECK(!app.session.isArmed(0));
        CHECK(app.playA()[0] > 0.0f); // In: plays, disarmed
    }

    SECTION("5: showing and hiding the mixer, its I/O and its sends")
    {
        CHECK(app.ctrlAlt('m'));
        CHECK(!view.isMixerVisible());
        CHECK(app.ctrlAlt('m'));
        CHECK(view.isMixerVisible());
        CHECK(app.ctrlAlt('i'));
        CHECK(!view.isIOVisible());
        CHECK(!view.stripForColumn(0)->inputBox()->isVisible());
        CHECK(app.ctrlAlt('i'));
        CHECK(view.stripForColumn(0)->inputBox()->isVisible());
        CHECK(app.ctrlAlt('s'));
        CHECK(!view.areSendsVisible());
        CHECK(!view.stripForColumn(0)->sendKnobs()[0]->isVisible());
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
    auto peaks = app.playA();
    CHECK(peaks[0] > 0.0f);
    CHECK(peaks[1] == 0.0f);

    // 4: track 2 selected: A plays it instead.
    app.clickHeader(1);
    CHECK(app.session.isAutoArmed(1));
    CHECK(!app.session.isArmed(0));
    peaks = app.playA();
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

    // On a track it keeps its usual default.
    debug.showChainOf({debugui::ChainOwner::Kind::Track, 0});
    debug.detailView().chainRow()->insertFromDescription(debugui::drag::device(ReverbDevice::typeId), -1);
    auto trackChain = LiveSet::chainOf(app.set().track(0));
    REQUIRE(trackChain.getNumChildren() == 2);
    CHECK(LiveSet::deviceParameter(trackChain.getChild(1), "reverb.mix", -1.0) == -1.0); // the default
}
