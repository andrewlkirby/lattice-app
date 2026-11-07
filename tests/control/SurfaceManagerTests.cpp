// Control surfaces in the app (docs/06-m5-controllers.md, 5e): the engine giving a surface port's
// messages to its surface first and the rest to MIDI Map (and to the tracks only when passed on);
// surfaces kept in the settings, connecting as their ports come and letting go as they go;
// recognised devices offered once, or never again; user definitions; the Surface Monitor.

#include "support/OfflineRenderer.h"
#include "support/Snapshots.h"
#include "support/TestDevices.h"
#include "ui/commands/SessionCommands.h"
#include "ui/controllers/LearnWizard.h"
#include "ui/controllers/SurfaceManager.h"
#include "ui/prefs/PreferencesView.h"
#include "ui/shell/MainShell.h"

#include <catch2/catch_test_macros.hpp>

using namespace lattice;
using namespace lattice::control;
using namespace lattice::control::generic;
using namespace lattice::test;
using ui::SurfaceManager;
using ui::MainShell;
using ui::PreferencesView;
using ui::ShellActions;

namespace
{
struct FakePorts final : ui::MidiPorts
{
    struct Sender final : MidiSender
    {
        explicit Sender(std::vector<juce::MidiMessage>& s) : sent(s) {}
        void send(const juce::MidiMessage& m) override { sent.push_back(m); }
        std::vector<juce::MidiMessage>& sent;
    };
    juce::Array<juce::MidiDeviceInfo> ins, outs;
    juce::StringArray open;
    std::vector<juce::MidiMessage> sent;

    juce::Array<juce::MidiDeviceInfo> inputs() const override { return ins; }
    juce::Array<juce::MidiDeviceInfo> outputs() const override { return outs; }
    std::unique_ptr<MidiSender> openOutput(const juce::String&) override { return std::make_unique<Sender>(sent); }
    void setInputOpen(const juce::String& id, bool on) override
    {
        if (on)
            open.addIfNotAlreadyThere(id);
        else
            open.removeString(id);
    }
    bool isInputOpen(const juce::String& id) const override { return open.contains(id); }

    void plug(const juce::String& name)
    {
        ins.add({name, "in:" + name});
        outs.add({name, "out:" + name});
        if (onChange)
            onChange();
    }
    void unplug(const juce::String& name)
    {
        ins.removeIf([&](const auto& d) { return d.name == name; });
        outs.removeIf([&](const auto& d) { return d.name == name; });
        if (onChange)
            onChange();
    }
};

struct Fixture
{
    Fixture()
    {
        settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lattice-surfaces-" + juce::Uuid().toString() + ".settings");
        juce::PropertiesFile::Options options;
        options.applicationName = "LatticeTests";
        settings = std::make_unique<juce::PropertiesFile>(settingsFile, options);
        folder = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lattice-surface-defs-" + juce::Uuid().toString());
        folder.createDirectory();
        session.newSet(4, 4);
        session.set().createClip(0, 0, 4.0);
        session.flush();
        ui::addSessionCommands(commands, session, {[this] { return selection.get(); }, [this](ui::SessionSelection s) { selection.set(s); }}, {});
        make();
    }
    ~Fixture()
    {
        manager.reset();
        settings.reset();
        settingsFile.deleteFile();
        folder.deleteRecursively();
    }
    void make()
    {
        manager.reset();
        manager = std::make_unique<SurfaceManager>(engine, *settings, ports,
                                                   [this] { return SurfaceContext{session, commands, selection, feed}; }, folder);
        manager->onOffer = [this](const SurfaceManager::Offer& o) { offers.push_back(o); };
    }
    /** What reached the tracks since last asked. */
    int toTracks()
    {
        std::array<TimedMidi, 256> buffer;
        return engine.midiInput().popInto(buffer.data(), static_cast<int>(buffer.size()));
    }
    juce::MidiMessage lpxPad(int note, int velocity = 100) { return juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(velocity)); }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer{engine, BlockPattern::Fixed64};
    ui::EngineStateFeed feed{session, engine};
    ui::CommandRegistry commands{&session.undoManager()};
    ui::SessionSelectionModel selection;
    juce::File settingsFile, folder;
    std::unique_ptr<juce::PropertiesFile> settings;
    FakePorts ports;
    std::unique_ptr<SurfaceManager> manager;
    std::vector<SurfaceManager::Offer> offers;
};
} // namespace

TEST_CASE("Surface ports: the surface's own messages stop there; the rest go to MIDI Map, and notes to the tracks only if passed on", "[control][generic][surfaces]")
{
    Fixture f;
    struct Sink final : AudioEngine::SurfaceSink
    {
        bool receive(const juce::MidiMessage& m, double) override
        {
            ++got;
            return m.isNoteOnOrOff() && m.getNoteNumber() == 60;
        }
        int got = 0;
    } sink;
    f.engine.setSurfaceSink("dev", &sink, false);
    CHECK(f.engine.isSurfaceInput("dev"));
    f.toTracks();
    f.engine.receiveMidi("dev", juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100))); // its own
    f.engine.receiveMidi("dev", juce::MidiMessage::noteOn(1, 61, static_cast<juce::uint8>(100))); // not: MIDI Map's
    f.engine.receiveMidi("dev", juce::MidiMessage::controllerEvent(1, 7, 90));
    CHECK(sink.got == 3);
    auto remote = f.engine.takeRemoteMidi();
    REQUIRE(remote.size() == 2);
    CHECK(remote[0].message.getNoteNumber() == 61);
    CHECK(f.toTracks() == 0); // nothing to the tracks
    // Passed on: what isn't the surface's reaches the tracks too.
    f.engine.setSurfaceSink("dev", &sink, true);
    f.engine.receiveMidi("dev", juce::MidiMessage::noteOn(1, 62, static_cast<juce::uint8>(100)));
    f.engine.receiveMidi("dev", juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)));
    CHECK(f.toTracks() == 1);
    f.engine.setSurfaceSink("dev", nullptr, false);
    CHECK_FALSE(f.engine.isSurfaceInput("dev"));
    // Another input is untouched.
    f.engine.receiveMidi("keys", juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(100)));
    CHECK(f.toTracks() == 1);
}

TEST_CASE("Surface manager: a recognised device is offered once; added, it connects, plays and keeps in the settings", "[control][generic][surfaces]")
{
    Fixture f;
    CHECK(f.manager->definitions().size() == 10);
    f.manager->refreshPorts();
    CHECK(f.offers.empty());

    // The Launchpad X comes: offered, with its own ports.
    f.ports.plug("LPX MIDI");
    REQUIRE(f.offers.size() == 1);
    CHECK(f.offers[0] == SurfaceManager::Offer{"Novation Launchpad X", "LPX MIDI", "LPX MIDI"});
    f.manager->refreshPorts();
    CHECK(f.offers.size() == 1); // once a session

    const int i = f.manager->add({f.offers[0].definition, f.offers[0].input, f.offers[0].output, false, true});
    REQUIRE(f.manager->isConnected(i));
    CHECK(f.engine.isSurfaceInput("in:LPX MIDI"));
    CHECK(f.ports.isInputOpen("in:LPX MIDI"));
    REQUIRE(!f.ports.sent.empty());
    CHECK(f.ports.sent[0].isSysEx()); // programmer mode
    CHECK(f.manager->controller(i) != nullptr);

    // Its top-left pad launches the first clip, through the engine's routing.
    f.session.play();
    f.renderer.renderFor(64);
    f.engine.receiveMidi("in:LPX MIDI", f.lpxPad(81));
    f.manager->driver(i)->deliverPending();
    f.session.flush();
    f.renderer.renderFor(64);
    f.session.processEngineEvents();
    f.feed.tick(5.0);
    CHECK(f.feed.slot(0, 0) != SlotState::Stopped);
    CHECK(f.engine.takeRemoteMidi().empty()); // its own: not MIDI Map's

    // Kept: a new manager connects it again.
    f.offers.clear();
    f.make();
    REQUIRE(f.manager->entries().size() == 1);
    CHECK(f.manager->isConnected(0));
    CHECK(f.offers.empty());

    // Unplugged, it lets go; plugged back, it connects.
    f.ports.unplug("LPX MIDI");
    CHECK_FALSE(f.manager->isConnected(0));
    CHECK_FALSE(f.engine.isSurfaceInput("in:LPX MIDI"));
    f.ports.plug("LPX MIDI");
    CHECK(f.manager->isConnected(0));
    CHECK(f.offers.empty()); // it has a surface: not offered

    // Removed: its port is a plain input again, and the definition is offered once more.
    f.ports.sent.clear();
    f.manager->remove(0);
    CHECK(f.manager->entries().empty());
    CHECK_FALSE(f.engine.isSurfaceInput("in:LPX MIDI"));
    CHECK_FALSE(f.ports.isInputOpen("in:LPX MIDI")); // the surface opened it
    REQUIRE(!f.ports.sent.empty());
    CHECK(f.ports.sent.back().isSysEx()); // out of programmer mode
}

TEST_CASE("Surface manager: declined for good; an input already open stays open; templates are never offered", "[control][generic][surfaces]")
{
    Fixture f;
    f.ports.plug("APC40 mkII");
    REQUIRE(f.offers.size() == 1);
    f.manager->decline(f.offers[0]);
    f.make();
    f.offers.clear();
    f.manager->refreshPorts();
    CHECK(f.offers.empty());

    // A keyboard: no definition matches; the template can be put on it by hand, notes passed on.
    f.ports.setInputOpen("in:Keystation", true); // the tracks' already
    f.ports.plug("Keystation");
    CHECK(f.offers.empty());
    const int i = f.manager->add({"Keyboard + 8 knobs", "Keystation", "Keystation", true, true});
    REQUIRE(f.manager->isConnected(i));
    f.toTracks();
    f.engine.receiveMidi("in:Keystation", juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)));
    CHECK(f.toTracks() == 1);
    f.manager->remove(i);
    CHECK(f.ports.isInputOpen("in:Keystation")); // not the surface's to close
}

TEST_CASE("Surface manager: the user's definitions, a broken one named; the monitor says what each message is", "[control][generic][surfaces]")
{
    Fixture f;
    f.folder.getChildFile("Mine.surface.json")
        .replaceWithText(R"({"lattice": "surface", "version": 1, "name": "My Pads", "ports": {"input": ["MyPads"]},
                            "grid": {"rows": 2, "columns": 2, "first": 36, "rowStep": -2}})");
    f.folder.getChildFile("Broken.surface.json").replaceWithText(R"({"lattice": "surface", "version": 1, "name": "x", "ports": {"input": ["y"]}})");
    f.manager->reloadDefinitions();
    CHECK(f.manager->definition("My Pads") != nullptr);
    REQUIRE(f.manager->problems().size() == 1);
    CHECK(f.manager->problems()[0].startsWith("Broken.surface.json: the file: no grid"));

    f.ports.plug("MyPads");
    REQUIRE(f.offers.size() == 1);
    const int i = f.manager->add({"My Pads", "MyPads", "MyPads", false, true});
    std::vector<SurfaceManager::Seen> seen;
    f.manager->setMonitor([&](const SurfaceManager::Seen& s) { seen.push_back(s); });
    f.engine.receiveMidi("in:MyPads", juce::MidiMessage::noteOn(1, 34, static_cast<juce::uint8>(100))); // top-left is 36: this is row 2
    f.engine.receiveMidi("in:MyPads", juce::MidiMessage::controllerEvent(1, 1, 64));
    f.manager->deliverMonitor();
    REQUIRE(seen.size() == 2);
    CHECK(seen[0].entry == i);
    CHECK(seen[0].meaning == "pad row 2, column 1");
    CHECK(seen[1].meaning.isEmpty()); // passed on
    f.manager->setMonitor(nullptr);
}

TEST_CASE("Surfaces in the window: Preferences lists them; their ports' MIDI switches are the surface's", "[control][generic][surfaces][ui]")
{
    Fixture f;
    f.manager.reset();
    auto ports = std::make_unique<FakePorts>();
    auto* fake = ports.get();
    MainShell shell(f.session, f.engine, *f.settings, f.tunings, ShellActions{});
    shell.setSize(1280, 900);
    shell.startControlSurfaces(std::move(ports));
    auto* manager = shell.surfaceManager();
    REQUIRE(manager != nullptr);
    std::vector<SurfaceManager::Offer> offers;
    manager->onOffer = [&](const SurfaceManager::Offer& o) { offers.push_back(o); };
    fake->plug("LPX MIDI");
    REQUIRE(offers.size() == 1);
    manager->add({offers[0].definition, offers[0].input, offers[0].output, false, true});

    PreferencesView view(shell.preferencesContext());
    view.setSize(1000, 900);
    view.show(PreferencesView::Page::controllers);
    std::function<juce::Component*(juce::Component&, const juce::String&)> titled = [&](juce::Component& root, const juce::String& start) -> juce::Component* {
        for (auto* c : root.getChildren())
        {
            if (c->getTitle().startsWith(start))
                return c;
            if (auto* deeper = titled(*c, start))
                return deeper;
        }
        return nullptr;
    };
    REQUIRE(view.pageComponent() != nullptr);
    CHECK(titled(*view.pageComponent(), "Control surfaces") != nullptr);
    auto* row = titled(*view.pageComponent(), "Novation Launchpad X (unverified)");
    REQUIRE(row != nullptr);
    CHECK(row->getTitle().contains("connected"));
    CHECK(row->getTitle().contains("LPX MIDI"));
    CHECK(row->isVisible());
    CHECK(!row->getBounds().isEmpty());
    if (const auto dir = juce::SystemStats::getEnvironmentVariable("LATTICE_WALKTHROUGH_DIR", {}); dir.isNotEmpty())
        writePng(snapshot(view, 1.0f), juce::File(dir).getChildFile("prefs-surfaces.png")); // to look at

    // Unplugged, its line says so.
    fake->unplug("LPX MIDI");
    row = titled(*view.pageComponent(), "Novation Launchpad X (unverified)");
    REQUIRE(row != nullptr);
    CHECK(row->getTitle().contains("not connected"));
}

TEST_CASE("Learn wizard: it listens to the chosen input only, saves a file the manager reads, and puts the surface on", "[control][generic][surfaces][learn]")
{
    Fixture f;
    f.ports.plug("Mystery Pads");
    f.offers.clear();
    bool finished = false;
    ui::LearnWizard wizard(*f.manager, [&] { finished = true; });
    const auto shot = [&](const juce::String& name) {
        if (const auto dir = juce::SystemStats::getEnvironmentVariable("LATTICE_WALKTHROUGH_DIR", {}); dir.isNotEmpty())
            writePng(snapshot(wizard, 1.0f), juce::File(dir).getChildFile("learn-" + name + ".png")); // to look at
    };
    shot("1-setup");
    REQUIRE(wizard.begin("Mystery", "Mystery Pads", "Mystery Pads", 2, 2));
    CHECK(f.engine.isSurfaceInput("in:Mystery Pads")); // the wizard's while it learns
    CHECK(f.ports.isInputOpen("in:Mystery Pads"));
    const auto send = [&](const juce::MidiMessage& m) {
        f.engine.receiveMidi("in:Mystery Pads", m);
        f.manager->deliverLearning();
    };
    for (const int n : {50, 51, 54})
        send(juce::MidiMessage::noteOn(2, n, static_cast<juce::uint8>(100)));
    CHECK(f.toTracks() == 0); // learning: nothing reaches the tracks
    shot("2-encoders");
    REQUIRE(wizard.learner().step() == ui::SurfaceLearner::Step::encoders);
    wizard.learner().encodersFinished();
    send(juce::MidiMessage::controllerEvent(2, 20, 127));
    REQUIRE(wizard.learner().hasPendingButton());
    shot("3-button");
    REQUIRE(wizard.learner().assign({Role::shift, 0}));
    wizard.learner().buttonsFinished();
    wizard.resized();
    wizard.repaint();
    shot("4-lights");
    wizard.learner().chooseLights(2);

    const auto file = wizard.save(true);
    REQUIRE(file.existsAsFile());
    CHECK(file.getParentDirectory() == f.folder);
    CHECK(file.getFileName() == "Mystery.surface.json");
    REQUIRE(f.manager->definition("Mystery") != nullptr);
    CHECK_FALSE(f.manager->definition("Mystery")->verified);
    REQUIRE(f.manager->entries().size() == 1);
    CHECK(f.manager->isConnected(0)); // the wizard let the input go, the surface has it
    // The learned grid plays: the bottom-right pad (54).
    f.engine.receiveMidi("in:Mystery Pads", juce::MidiMessage::noteOn(2, 54, static_cast<juce::uint8>(100)));
    CHECK(f.engine.takeRemoteMidi().empty());
}

TEST_CASE("Surface manager: a surface keeping its own selection keeps it in the settings, and connects that way",
          "[control][generic][surfaces]")
{
    Fixture f;
    f.ports.plug("LPX MIDI");
    const int i = f.manager->add({"Novation Launchpad X", "LPX MIDI", "LPX MIDI", false, true});
    REQUIRE(f.manager->controller(i) != nullptr);
    CHECK(f.manager->controller(i)->followsSelection());
    f.manager->setFollowsSelection(i, false);
    CHECK_FALSE(f.manager->controller(i)->followsSelection());
    CHECK_FALSE(f.manager->entries()[0].followsSelection);
    f.make(); // the next start
    REQUIRE(f.manager->controller(0) != nullptr);
    CHECK_FALSE(f.manager->entries()[0].followsSelection);
    CHECK_FALSE(f.manager->controller(0)->followsSelection());
}
