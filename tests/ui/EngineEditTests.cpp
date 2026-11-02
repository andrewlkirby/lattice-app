// Edits reach the engine (docs/05-m4-ui.md, Tests, item 12): the shell's commands, the mixer's
// controls, the piano roll and the device panels, used while the engine renders offline, each
// change the sound (or the notes played) as it says. Edits are made between blocks of a render
// that's already going, not only before the first block (M3 hands-on finding).

#include "devices/effects/Compressor.h"
#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"
#include "ui/device/panels/PanelKit.h"
#include "ui/shell/MainShell.h"
#include "ui/widgets/BoundControl.h"
#include "ui/widgets/Toggle.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace lattice;
using namespace lattice::ui;
using namespace lattice::test;
using Catch::Approx;

namespace
{
struct Fixture
{
    explicit Fixture(int tracks = 2)
    {
        settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("lattice-engine-edits", ".settings");
        juce::PropertiesFile::Options options;
        options.applicationName = "LatticeTests";
        settings = std::make_unique<juce::PropertiesFile>(settingsFile, options);
        session.newSet(tracks, 2);
        for (int t = 0; t < tracks; ++t)
        {
            auto chain = LiveSet::chainOf(session.set().track(t));
            while (chain.getNumChildren() > 0)
                session.set().removeDevice(chain.getChild(0));
        }
        session.flush();
        shell = std::make_unique<MainShell>(session, engine, *settings, tunings, ShellActions{});
        shell->setSize(1600, 1000);
        shell->setDetailVisible(true);
        renderer.captureAudio = true;
        settle();
    }
    ~Fixture()
    {
        shell.reset();
        settings.reset();
        settingsFile.deleteFile();
    }

    LiveSet& set() { return session.set(); }
    void settle()
    {
        session.flush();
        shell->sessionView().flushUpdates();
        shell->deviceView().flushUpdates();
        shell->clipView().flushUpdates();
        renderer.attach(session);
    }
    /** The binding of a control as the UI shows it (a knob, fader, box or toggle). */
    static ValueBinding* bindingOf(juce::Component* c)
    {
        if (auto* bound = dynamic_cast<BoundControl*>(c))
            return &bound->binding();
        if (auto* toggle = dynamic_cast<Toggle*>(c))
            return toggle->binding();
        return nullptr;
    }
    /** The control bound to an address (ValueBinding::target), or by its name for those without
        one (the set's own: tempo). */
    ValueBinding& control(const juce::String& targetOrName)
    {
        settle();
        for (const auto* e : shell->controlRegistry().entries())
            if (auto* b = bindingOf(e->component.getComponent()); b != nullptr && (b->target == targetOrName || (b->target.isEmpty() && b->spec().name == targetOrName)))
                return *b;
        FAIL("No control for " << targetOrName);
        throw std::logic_error("unreachable");
    }
    /** Renders on (as the engine keeps playing) and returns the output's peak over the second half. */
    float level(int samples = 9600)
    {
        settle();
        renderer.audioLeft.clear();
        renderer.renderFor(samples);
        float peak = 0.0f;
        for (std::size_t i = renderer.audioLeft.size() / 2; i < renderer.audioLeft.size(); ++i)
            peak = std::max(peak, std::abs(renderer.audioLeft[i]));
        return peak;
    }
    juce::ValueTree insertOn(int track, const juce::String& type)
    {
        shell->selection().set({SessionSelection::Kind::track, track, 0});
        settle();
        REQUIRE(shell->deviceView().insertHere(drag::device(type)));
        settle();
        auto chain = LiveSet::chainOf(set().track(track));
        return chain.getChild(chain.getNumChildren() - 1);
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer{engine, BlockPattern::Fixed64};
    juce::File settingsFile;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<MainShell> shell;
};

float dbToGain(float db)
{
    return std::pow(10.0f, db / 20.0f);
}
} // namespace

TEST_CASE("Edits reach the engine: the mixer's controls, while it plays", "[ui][engine]")
{
    Fixture f;
    f.insertOn(0, TestSineDevice::typeId);
    const float alone = f.level();
    REQUIRE(alone > 0.05f);
    const auto track = f.set().track(0);

    SECTION("volume")
    {
        f.control(ValueBinding::targetOf(track, ids::volumeDb)).setValue(-12.0f);
        CHECK(f.level() == Approx(alone * dbToGain(-12.0f)).epsilon(0.05));
    }
    SECTION("Track On, and on again")
    {
        f.control(ValueBinding::targetOf(track, ids::mute)).setValue(0.0f); // "Track On" off
        CHECK(f.level() < 1e-4f);
        f.session.undoManager().undo();
        CHECK(f.level() == Approx(alone).epsilon(0.05));
    }
    SECTION("another track's solo")
    {
        f.control(ValueBinding::targetOf(f.set().track(1), ids::solo)).setValue(1.0f);
        CHECK(f.level() < 1e-4f);
    }
    SECTION("a send turned up reaches its return")
    {
        // The track's own fader down, its sends taken before it: only the return is heard.
        REQUIRE(f.set().numReturns() > 0);
        const auto ret = f.set().returnTrack(0);
        f.control(ValueBinding::targetOf(ret, ids::preFader)).setValue(1.0f);
        f.control(ValueBinding::targetOf(track, ids::volumeDb)).setValue(-70.0f);
        const auto send = "send:" + juce::String(LiveSet::idOf(track)) + "/" + juce::String(LiveSet::idOf(ret));
        f.control(send).setValue(-70.0f);
        CHECK(f.level() < alone * 0.01f);
        f.control(send).setValue(0.0f);
        CHECK(f.level() == Approx(alone).epsilon(0.1));
    }
}

TEST_CASE("Edits reach the engine: devices from the commands and their panels, while it plays", "[ui][engine]")
{
    Fixture f;
    f.insertOn(0, TestSineDevice::typeId);
    const float alone = f.level();

    // Inserted by the command (Device menu), then turned down on its panel's knob.
    REQUIRE(f.shell->commands().perform("device.insert.utility"));
    f.settle();
    const auto utility = LiveSet::chainOf(f.set().track(0)).getChild(1);
    REQUIRE(utility[ids::deviceType].toString() == "lattice.utility");
    const auto gain = "device:" + juce::String(LiveSet::idOf(utility)) + "/utility.gain";
    f.control(gain).setValue(-24.0f);
    CHECK(f.level() == Approx(alone * dbToGain(-24.0f)).epsilon(0.1));

    // Off by the command: the sound comes back; deleted and undone: down again.
    f.shell->deviceView().select(utility);
    REQUIRE(f.shell->commands().perform("device.onOff"));
    CHECK(f.level() == Approx(alone).epsilon(0.05));
    REQUIRE(f.shell->commands().perform("device.onOff"));
    CHECK(f.level() == Approx(alone * dbToGain(-24.0f)).epsilon(0.1));
}

TEST_CASE("Edits reach the engine: a sidechain source chosen in the picker ducks", "[ui][engine]")
{
    Fixture f;
    f.insertOn(0, TestSineDevice::typeId); // the source
    f.insertOn(1, TestSineDevice::typeId);
    const auto quieter = f.insertOn(1, "lattice.utility");
    f.control("device:" + juce::String(LiveSet::idOf(quieter)) + "/utility.gain").setValue(-30.0f); // under the threshold
    const auto comp = f.insertOn(1, "lattice.compressor");
    const auto compressor = "device:" + juce::String(LiveSet::idOf(comp));
    f.control(compressor + "/comp.threshold").setValue(-24.0f);
    f.control(compressor + "/comp.sidechain").setValue(1.0f); // its Sidechain switch on
    auto& feed = f.shell->feed();
    feed.watchDevice(LiveSet::idOf(comp));
    const auto reduction = [&] {
        for (int i = 0; i < 20; ++i)
        {
            f.level(800);
            feed.tick(1.0 + i / 60.0);
        }
        const auto* d = feed.device(LiveSet::idOf(comp));
        return d != nullptr ? d->gainReductionDb : 0.0f;
    };
    CHECK(reduction() > -1.0f); // its own track is quiet

    // Track 1 chosen in the picker's menu, as a click would.
    panels::SidechainPicker* picker = nullptr;
    f.settle();
    std::function<void(juce::Component&)> find = [&](juce::Component& in) {
        for (auto* child : in.getChildren())
        {
            if (auto* p = dynamic_cast<panels::SidechainPicker*>(child))
                picker = p;
            find(*child);
        }
    };
    find(f.shell->deviceView());
    REQUIRE(picker != nullptr);
    bool chose = false;
    const auto menu = picker->sourceMenu();
    for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
        if (it.getItem().text == f.set().track(0)[ids::name].toString() && it.getItem().action)
        {
            it.getItem().action();
            chose = true;
        }
    REQUIRE(chose);
    CHECK(reduction() < -6.0f);
    feed.unwatchDevice(LiveSet::idOf(comp));
}

TEST_CASE("Edits reach the engine: a clip made and edited in the UI plays, and changes as it plays", "[ui][engine]")
{
    Fixture f(1);
    auto& commands = f.shell->commands();
    f.shell->selection().set({SessionSelection::Kind::slot, 0, 0});
    REQUIRE(commands.perform("create.midiClip"));
    f.settle();
    const auto clip = f.set().clip(0, 0);
    REQUIRE(clip.isValid());

    // A note drawn in the piano roll, the clip launched and the transport started.
    auto& roll = f.shell->clipView().pianoRoll();
    REQUIRE(roll.hasClip());
    roll.doubleClick({static_cast<float>(roll.noteArea().getX() + 4), static_cast<float>(roll.noteArea().getCentreY())});
    f.settle();
    const auto notes = clip.getChildWithName(ids::notes);
    REQUIRE(notes.getNumChildren() == 1);
    const int pitch = notes.getChild(0)[ids::pitch];
    REQUIRE(commands.perform("session.launch"));
    if (!f.session.transport().playing)
        REQUIRE(commands.perform("transport.play"));
    f.settle();
    const auto trackId = LiveSet::idOf(f.set().track(0));
    const auto loop = OfflineRenderer::samplesForBeats(4.0, f.set().tempo());
    f.renderer.renderFor(loop * 2);
    auto played = f.renderer.noteOns(trackId);
    REQUIRE(!played.empty());
    CHECK(played.back().data1 == pitch);

    // Moved up a step while it plays: the next loop plays the new note.
    roll.selectAll();
    roll.transposeSteps(1);
    f.settle();
    const int moved = notes.getChild(0)[ids::pitch];
    REQUIRE(moved != pitch);
    f.renderer.events.clear();
    f.renderer.renderFor(loop * 2);
    played = f.renderer.noteOns(trackId);
    REQUIRE(!played.empty());
    CHECK(played.back().data1 == moved);

    // The tempo doubled on the control bar: the loop comes round twice as often.
    f.renderer.events.clear();
    f.renderer.renderFor(loop * 4);
    const auto before = f.renderer.noteOns(trackId).size();
    f.control("Tempo").setValue(static_cast<float>(f.set().tempo() * 2.0));
    REQUIRE(f.set().tempo() == Approx(240.0));
    f.settle(); // as the session's timer would, between blocks
    f.renderer.events.clear();
    f.renderer.renderFor(loop * 4);
    CHECK(f.renderer.noteOns(trackId).size() == Approx(before * 2.0).margin(1.0));

    // Stopped from the track's stop button command: nothing more plays.
    REQUIRE(commands.perform("session.stopTrack"));
    f.renderer.renderFor(loop); // up to the quantized stop
    f.renderer.events.clear();
    f.renderer.renderFor(loop * 2);
    CHECK(f.renderer.noteOns(trackId).empty());
    CHECK(f.renderer.unbalancedNotes(trackId).empty());
}
