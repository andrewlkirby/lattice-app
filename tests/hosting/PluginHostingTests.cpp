// Plugins as devices (docs/07-m6-plugins-external.md, 6b; "Tests" 2): the CLAP host, a plugin loading
// in the background behind a stand-in and then playing on a track, its state and pages through save
// and reload, a missing plugin kept exactly and restored, the plugin's own changes in the document
// (Configure mode included), latency changes followed by delay compensation, sidechain, MIDI out,
// per-note expression, and a plugin's allocations counted without being Lattice's violations.

#include "devices/ChainEditing.h"
#include "devices/framework/ParameterPager.h"
#include "hosting/PluginHost.h"
#include "hosting/clap/ClapFormat.h"
#include "hosting/clap/ClapInstance.h"
#include "support/OfflineRenderer.h"
#include "support/Spectrum.h"
#include "support/TestDevices.h"
#include "support/TestFiles.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>

using namespace lattice;
using namespace lattice::hosting;
using namespace lattice::test;

namespace juce::detail
{
bool dispatchNextMessageOnSystemQueue(bool returnIfNoPendingMessages); // juce_events' own message pump
}

namespace
{
const juce::File testClap{LATTICE_TEST_CLAP};

juce::File testVst3() { return juce::File(LATTICE_TEST_VST3_BINARY_DIR).getParentDirectory().getParentDirectory(); }

/** Runs the message loop until `done` or `seconds` have passed. */
bool pumpUntil(const std::function<bool()>& done, double seconds)
{
    const auto until = juce::Time::getMillisecondCounterHiRes() + seconds * 1000.0;
    while (!done() && juce::Time::getMillisecondCounterHiRes() < until)
        if (!juce::detail::dispatchNextMessageOnSystemQueue(true))
            juce::Thread::sleep(2);
    return done();
}

scan::ScannedPlugin clapPlugin(const juce::String& name, const juce::File& file = testClap)
{
    juce::String error;
    for (auto& p : ClapPluginFormat::scanFile(file.getFullPathName(), error))
        if (p.description.name == name)
            return p;
    FAIL("no " << name << " in " << file.getFullPathName() << ": " << error);
    return {};
}

scan::ScannedPlugin vst3Plugin()
{
    juce::VST3PluginFormat format;
    juce::OwnedArray<juce::PluginDescription> found;
    format.findAllTypesForFile(found, testVst3().getFullPathName());
    REQUIRE(found.size() == 1);
    scan::ScannedPlugin p;
    p.description = *found[0];
    return p;
}

/** A session with a plugin host (no library: plugins are where they were saved from), the test
    devices, and an OfflineRenderer. Tracks start empty. Nothing may allocate on the audio thread
    but the plugins themselves. */
struct Fixture
{
    explicit Fixture(int numTracks = 1) : renderer(engine, BlockPattern::Fixed64)
    {
        session.newSet(numTracks, 2);
        for (int t = 0; t < numTracks; ++t)
        {
            auto chain = LiveSet::chainOf(set().track(t));
            while (chain.getNumChildren() > 0)
                set().removeDevice(chain.getChild(0));
        }
        session.flush();
        violationsBefore = rt::violationCount();
    }
    ~Fixture() { CHECK(rt::violationCount() == violationsBefore); }

    LiveSet& set() { return session.set(); }
    juce::ValueTree chain(int track = 0) { return LiveSet::chainOf(set().track(track)); }

    /** Puts a plugin on a track, as the browser does; it loads with load(). */
    juce::ValueTree insert(const scan::ScannedPlugin& plugin, int track = 0)
    {
        session.beginEdit("Insert", chain(track));
        auto d = chains::insert(set(), chain(track), PluginHost::typeFor(plugin), -1, session.deviceRegistry());
        REQUIRE(d.isValid());
        PluginHost::describeDevice(set(), d, plugin);
        session.flush();
        return d;
    }
    void load() { host.finishLoading(); }
    HostedPluginDevice* live(const juce::ValueTree& device) { return host.liveDevice(LiveSet::idOf(device)); }
    StudioDevice* instance(const juce::ValueTree& device) { return session.snapshots().deviceFor(LiveSet::idOf(device)); }
    void render(std::int64_t samples)
    {
        renderer.renderFor(samples);
        host.idle();
        session.flush();
        session.snapshots().collectGarbage();
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    PluginHost host{session, nullptr};
    OfflineRenderer renderer;
    std::uint64_t violationsBefore = 0;
};

/** Runs a device alone, as a chain does, for `samples`, and returns its left output. */
std::vector<float> runAlone(StudioDevice& device, int samples, const std::vector<TimedMidi>& firstBlockMidi = {},
                            float input = 0.0f, const float* sidechainLevel = nullptr, std::vector<TimedMidi>* midiOut = nullptr)
{
    constexpr int block = 64;
    std::vector<float> out;
    std::vector<float> left(block), right(block), sideL(block), sideR(block);
    std::vector<TimedMidi> inStore(512), outStore(512);
    for (int done = 0; done < samples; done += block)
    {
        std::fill(left.begin(), left.end(), input);
        std::fill(right.begin(), right.end(), input);
        if (sidechainLevel != nullptr)
        {
            std::fill(sideL.begin(), sideL.end(), *sidechainLevel);
            std::fill(sideR.begin(), sideR.end(), *sidechainLevel);
        }
        MidiBlock in{inStore.data(), static_cast<int>(inStore.size()), 0, 0};
        MidiBlock outBlock{outStore.data(), static_cast<int>(outStore.size()), 0, 0};
        if (done == 0)
            for (const auto& e : firstBlockMidi)
                in.add(e);
        float* channels[2]{left.data(), right.data()};
        const float* side[2]{sideL.data(), sideR.data()};
        DeviceBuffers io;
        io.channels = channels;
        io.numSamples = block;
        io.midiIn = &in;
        io.midiOut = &outBlock;
        io.sidechain = sidechainLevel != nullptr ? side : nullptr;
        DeviceTimeInfo time;
        {
            const rt::ScopedAudioThread audio;
            device.processInChain(io, time);
        }
        if (midiOut != nullptr)
            for (const auto& e : outBlock)
                midiOut->push_back(e);
        out.insert(out.end(), left.begin(), left.end());
    }
    return out;
}

TimedMidi midi(std::uint8_t status, std::uint8_t d1, std::uint8_t d2, std::uint8_t flags = 0)
{
    TimedMidi e;
    e.bytes[0] = status;
    e.bytes[1] = d1;
    e.bytes[2] = d2;
    e.size = 3;
    e.flags = flags;
    return e;
}

std::unique_ptr<HostedPluginDevice> hostedAlone(const juce::String& name)
{
    juce::String error;
    auto plugin = ClapPluginInstance::create(clapPlugin(name).description, 48000.0, 64, error);
    INFO(error);
    REQUIRE(plugin != nullptr);
    const auto type = PluginHost::typeFor(clapPlugin(name));
    auto device = std::make_unique<HostedPluginDevice>(1, type, std::move(plugin));
    device->prepareDevice(48000.0, 64);
    return device;
}
} // namespace

// --- The CLAP host -------------------------------------------------------------------------------------------

TEST_CASE("A CLAP plugin is hosted: its parameters and groups, notes, text and state", "[hosting][plugins]")
{
    juce::String error;
    auto plugin = ClapPluginInstance::create(clapPlugin("Lattice Test Plugin").description, 48000.0, 512, error);
    INFO(error);
    REQUIRE(plugin != nullptr);
    CHECK(plugin->getName() == "Lattice Test Plugin");
    CHECK(plugin->acceptsMidi());
    CHECK(plugin->takesClapNotes());
    CHECK(plugin->getTotalNumOutputChannels() == 2);

    const auto& parameters = plugin->getParameters();
    REQUIRE(parameters.size() == 3);
    auto* level = parameters[0];
    CHECK(level->getName(64) == "Level");
    CHECK(level->getValue() == Catch::Approx(0.25f));
    CHECK(level->getText(0.5f, 64) == "50 %");
    CHECK(level->isAutomatable());
    CHECK(plugin->getParameterTree().getGroupsForParameter(level).getFirst()->getName() == "Output");
    CHECK(parameters[1]->getNumSteps() == 3);
    CHECK(parameters[1]->getText(1.0f, 64) == "Saw");
    CHECK_FALSE(parameters[2]->isAutomatable()); // hidden

    plugin->prepareToPlay(48000.0, 512);
    juce::AudioBuffer<float> buffer(2, 512);
    juce::MidiBuffer notes;
    notes.addEvent(juce::MidiMessage::noteOn(1, 69, 1.0f), 0);
    level->setValue(0.8f); // reaches it as an event in the block
    {
        const rt::ScopedAudioThread audio;
        plugin->processBlock(buffer, notes);
    }
    CHECK(buffer.getMagnitude(0, 0, 512) == Catch::Approx(0.8f).margin(0.01f));

    juce::MemoryBlock state;
    plugin->getStateInformation(state);
    CHECK(state.toString().startsWith("LTPS"));
    auto another = ClapPluginInstance::create(clapPlugin("Lattice Test Plugin").description, 48000.0, 512, error);
    REQUIRE(another != nullptr);
    another->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    CHECK(another->getParameters()[0]->getValue() == Catch::Approx(0.8f));
    plugin->releaseResources();
}

// --- Loading, playing, saving -----------------------------------------------------------------------------------

TEST_CASE("A plugin on a track loads behind a stand-in, then plays", "[hosting][plugins]")
{
    Fixture f;
    const bool clap = GENERATE(true, false);
    const auto plugin = clap ? clapPlugin("Lattice Test Plugin") : vst3Plugin();
    INFO(plugin.description.pluginFormatName);
    const auto device = f.insert(plugin);
    CHECK(device[ids::deviceType].toString() == pluginTypes::instrument);
    CHECK(device[ids::name].toString() == "Lattice Test Plugin");

    // Until it's loaded: a silent stand-in, and the window doesn't wait for it.
    const auto* standIn = dynamic_cast<const PluginStandIn*>(f.instance(device));
    REQUIRE(standIn != nullptr);
    CHECK(standIn->why() == PluginStandIn::Why::loading);
    CHECK(f.host.statusOf(device).state == PluginStatus::State::loading);
    CHECK(f.host.loadsWaiting() == 1);

    f.load();
    auto* hosted = f.live(device);
    REQUIRE(hosted != nullptr);
    CHECK(f.instance(device) == hosted);
    CHECK(f.host.statusOf(device).state == PluginStatus::State::ready);

    // Its pages: the parameters a host may automate, in order (not CLAP's hidden one).
    const auto pages = juce::StringArray::fromTokens(device.getChildWithName(ids::deviceState)[pluginTypes::configured].toString(), " ", {});
    REQUIRE(pages.size() >= 1);
    CHECK(hosted->parameter(hosted->parameterIndex(pages[0])).spec().name == "Level");
    if (clap)
        CHECK(pages.joinIntoString(" ") == "p10 p20");
    // ...in the document, at the plugin's values.
    CHECK(LiveSet::deviceParameter(device, pages[0], -1.0) == Catch::Approx(0.25));
    const auto* described = f.host.describe(device);
    REQUIRE(described != nullptr);
    CHECK(described->numParameters() == pages.size());
    CHECK(described->parameter(0).spec().format(0.5f) == (clap ? "50 %" : described->parameter(0).spec().format(0.5f)));

    addClip(f.set(), 0, 0, 8.0, {NoteData{69, 0.0, 8.0, 127, false}});
    f.session.launchSlot(0, 0);
    f.renderer.captureAudio = true;
    f.render(OfflineRenderer::samplesForBeats(0.25));
    f.renderer.audioLeft.clear();
    f.render(1 << 16);
    const auto [hz, strength] = measureFundamental(f.renderer.audioLeft, 440.0);
    CHECK(hz == Catch::Approx(440.0).margin(0.5));
    CHECK(strength > 0.9);
}

TEST_CASE("A plugin's state and pages come back after saving and loading the set", "[hosting][plugins]")
{
    TempFolder folder;
    const auto file = folder / "plugin.lset";
    {
        Fixture f;
        const auto device = f.insert(clapPlugin("Lattice Test Plugin"));
        f.load();
        f.set().setDeviceParameter(device, "p10", 0.6);
        f.host.removeFromPages(device, "p20"); // Wave: off the pages, so the plugin's own (its state)
        f.live(device)->plugin().getParameters()[1]->setValueNotifyingHost(1.0f);
        f.render(256);
        REQUIRE(f.session.saveSet(file).wasOk());
        CHECK_FALSE(f.session.hasUnsavedChanges());
    }
    Fixture f;
    REQUIRE(f.session.loadSet(file).wasOk());
    f.load();
    const auto device = LiveSet::chainOf(f.set().track(0)).getChild(0);
    auto* hosted = f.live(device);
    REQUIRE(hosted != nullptr);
    f.render(256);
    CHECK(hosted->plugin().getParameters()[0]->getValue() == Catch::Approx(0.6f)); // from the document
    CHECK(hosted->plugin().getParameters()[1]->getValue() == Catch::Approx(1.0f)); // from its state
    CHECK(device.getChildWithName(ids::deviceState)[pluginTypes::configured].toString() == "p10");
    // Loading it doesn't change the set: no unsaved changes come from the plugin starting.
    for (int i = 0; i < 4; ++i)
        f.render(256);
    CHECK_FALSE(f.session.hasUnsavedChanges());
}

TEST_CASE("A missing plugin keeps its state and values exactly, and comes back when installed", "[hosting][plugins]")
{
    TempFolder folder;
    auto copy = folder / "Moved.clap";
    REQUIRE(testClap.copyFileTo(copy));
    std::unique_ptr<juce::XmlElement> saved;
    {
        Fixture f;
        const auto device = f.insert(clapPlugin("Lattice Test Plugin", copy));
        f.load();
        f.set().setDeviceParameter(device, "p10", 0.7);
        f.render(256);
        saved = f.set().toXml();
    }
    REQUIRE(copy.deleteFile()); // uninstalled: the last instance went, and the library with it

    Fixture f;
    const auto savedFile = folder / "missing.lset";
    REQUIRE(saved->writeTo(savedFile));
    REQUIRE(f.session.loadSet(savedFile).wasOk());
    f.session.flush();
    f.load();
    const auto device = LiveSet::chainOf(f.set().track(0)).getChild(0);
    CHECK(f.host.statusOf(device).state == PluginStatus::State::missing);
    CHECK(f.host.statusOf(device).message.contains("isn't installed"));
    const auto* standIn = dynamic_cast<const PluginStandIn*>(f.instance(device));
    REQUIRE(standIn != nullptr);
    CHECK(standIn->why() == PluginStandIn::Why::missing);
    // Saved again, it's what it was.
    const auto again = f.set().toXml();
    const auto deviceXml = [](const juce::XmlElement& set) {
        return juce::ValueTree::fromXml(set).getChildWithName(ids::tracks).getChild(0).getChildWithName(ids::devices).getChild(0);
    };
    CHECK(deviceXml(*again).isEquivalentTo(deviceXml(*saved)));

    // Installed again: it's tried again, and comes back as it was.
    REQUIRE(testClap.copyFileTo(copy));
    f.host.retryMissing();
    f.session.flush();
    f.load();
    auto* hosted = f.live(device);
    REQUIRE(hosted != nullptr);
    f.render(256);
    CHECK(hosted->plugin().getParameters()[0]->getValue() == Catch::Approx(0.7f));
}

TEST_CASE("Removing a plugin and undoing it brings it back with its state", "[hosting][plugins]")
{
    Fixture f;
    const auto device = f.insert(clapPlugin("Lattice Test Plugin"));
    f.load();
    f.host.removeFromPages(device, "p20");
    f.live(device)->plugin().getParameters()[1]->setValueNotifyingHost(1.0f); // in its state only
    f.render(256);

    f.session.beginEdit("Remove", device);
    f.set().removeDevice(device);
    f.render(256);
    f.render(256); // the engine lets go of it, then it goes
    CHECK(f.live(device) == nullptr);

    f.session.undoManager().undo();
    f.session.flush();
    f.load();
    auto* hosted = f.live(device);
    REQUIRE(hosted != nullptr);
    f.render(256);
    CHECK(hosted->plugin().getParameters()[1]->getValue() == Catch::Approx(1.0f));
}

// --- Pages and the plugin's own changes -----------------------------------------------------------------------

TEST_CASE("A plugin's groups become pages for any encoder count", "[hosting][plugins]")
{
    Fixture f;
    const auto device = f.insert(clapPlugin("Lattice Test Plugin"));
    f.load();
    const auto* described = f.host.describe(device);
    REQUIRE(described != nullptr);
    REQUIRE(described->parameterGroups().size() == 2);
    CHECK(described->parameterGroups()[0].name == "Output");
    CHECK(described->parameterGroups()[1].name == "Oscillator");
    for (const int encoders : {8, 16, 32})
    {
        const auto pages = ParameterPager::pagesFor(*described, encoders);
        int shown = 0;
        for (const auto& page : pages)
        {
            CHECK(static_cast<int>(page.parameters.size()) <= encoders);
            shown += static_cast<int>(page.parameters.size());
        }
        CHECK(shown == described->numParameters());
    }
    // The same description while nothing changed; a new one when the pages change.
    CHECK(f.host.describe(device) == described);
    f.host.removeFromPages(device, "p20");
    CHECK(f.host.describe(device)->numParameters() == 1);
}

TEST_CASE("The plugin's own changes reach the document: a gesture is one undo step", "[hosting][plugins]")
{
    Fixture f;
    const auto device = f.insert(clapPlugin("Lattice Test Plugin"));
    f.load();
    auto* hosted = f.live(device);
    REQUIRE(hosted != nullptr);
    auto& undo = f.session.undoManager();
    undo.clearUndoHistory();

    SECTION("From the plugin's audio thread (its automation out, or its window through it)")
    {
        // CC 20 makes the test plugin move Level itself, with a gesture, from process().
        REQUIRE(f.engine.midiInput().push(juce::MidiMessage::controllerEvent(1, 20, 127)));
        f.render(256);
        f.render(256);
        CHECK(LiveSet::deviceParameter(device, "p10", 0.0) == Catch::Approx(1.0));
        CHECK(undo.getNumActionsInCurrentTransaction() + (undo.canUndo() ? 1 : 0) >= 1);
        CHECK(undo.getUndoDescription().startsWith("Level"));
        undo.undo();
        f.render(256);
        CHECK(LiveSet::deviceParameter(device, "p10", 0.0) == Catch::Approx(0.25));
        CHECK(hosted->plugin().getParameters()[0]->getValue() == Catch::Approx(0.25f)); // sent back to it
    }
    SECTION("From its window on the message thread, in several moves")
    {
        auto* level = hosted->plugin().getParameters()[0];
        level->beginChangeGesture();
        for (const float v : {0.3f, 0.4f, 0.5f})
        {
            level->setValueNotifyingHost(v);
            f.render(64);
        }
        level->endChangeGesture();
        f.render(64);
        CHECK(LiveSet::deviceParameter(device, "p10", 0.0) == Catch::Approx(0.5));
        undo.undo();
        CHECK(LiveSet::deviceParameter(device, "p10", 0.0) == Catch::Approx(0.25)); // the whole gesture
    }
    SECTION("Configure: a parameter touched in the window joins the pages")
    {
        f.host.removeFromPages(device, "p20");
        CHECK_FALSE(device.getChildWithName(ids::params).hasProperty("p20"));
        hosted->plugin().getParameters()[1]->setValueNotifyingHost(0.5f); // not configuring: its own
        f.render(64);
        CHECK_FALSE(device.getChildWithName(ids::params).hasProperty("p20"));

        f.host.setConfiguring(LiveSet::idOf(device), true);
        auto* wave = hosted->plugin().getParameters()[1];
        wave->beginChangeGesture();
        wave->setValueNotifyingHost(1.0f);
        wave->endChangeGesture();
        f.render(64);
        CHECK(device.getChildWithName(ids::deviceState)[pluginTypes::configured].toString() == "p10 p20");
        CHECK(LiveSet::deviceParameter(device, "p20", -1.0) == Catch::Approx(2.0)); // Wave's choices: 0, 1, 2
    }
}

TEST_CASE("Pages saved as a plugin's default are what its new devices start with", "[hosting][plugins]")
{
    Fixture f;
    std::map<juce::String, juce::String> settings;
    f.host.storeDefaultPages = [&](const juce::String& key, const juce::String& pages) { settings[key] = pages; };
    f.host.loadDefaultPages = [&](const juce::String& key) { return settings[key]; };
    const auto first = f.insert(clapPlugin("Lattice Test Plugin"));
    f.load();
    f.host.removeFromPages(first, "p10");
    f.host.saveDefaultPages(first);
    const auto second = f.insert(clapPlugin("Lattice Test Plugin"));
    f.load();
    CHECK(second.getChildWithName(ids::deviceState)[pluginTypes::configured].toString() == "p20");
}

// --- Latency, sidechain, MIDI out, expression, diagnostics ------------------------------------------------------

TEST_CASE("A plugin's latency change reaches delay compensation", "[hosting][plugins]")
{
    Fixture f(2);
    SECTION("A CLAP plugin restarts to change it, keeping its state")
    {
        const auto device = f.insert(clapPlugin("Lattice Test Latency"));
        f.load();
        REQUIRE(f.live(device) != nullptr);
        f.render(256);
        CHECK(f.session.processorForTrack(0)->latencySamples() == 64);

        f.set().setDeviceParameter(device, "p1", 0.25); // 1000 of 0..4000: asks for a restart, as CLAP says it must
        for (int i = 0; i < 10 && f.session.processorForTrack(0)->latencySamples() != 1000; ++i)
        {
            f.render(256);
            f.host.finishLoading();
        }
        CHECK(f.session.processorForTrack(0)->latencySamples() == 1000);
        CHECK(f.live(device)->plugin().getParameters()[0]->getValue() == Catch::Approx(0.25f)); // a new device, its plugin as it was
        // The other track waits for it.
        f.render(256);
        CHECK(f.session.processorForTrack(1)->latencySamples() == 0);
    }
    SECTION("A VST3 plugin reports it")
    {
        const auto device = f.insert(vst3Plugin());
        f.load();
        auto* hosted = f.live(device);
        REQUIRE(hosted != nullptr);
        f.host.addToPages(device, hosted->parameter(1).id());
        f.set().setDeviceParameter(device, hosted->parameter(1).id(), 500.0 / 4000.0);
        CHECK(pumpUntil(
            [&] {
                f.render(64);
                return f.session.processorForTrack(0)->latencySamples() == 500;
            },
            5.0));
        CHECK(f.live(device) == hosted); // followed without a restart
    }
}

TEST_CASE("A plugin's sidechain input gets the sidechain", "[hosting][plugins]")
{
    auto device = hostedAlone("Lattice Test Sidechain");
    const float side = 0.25f;
    const auto out = runAlone(*device, 256, {}, 0.5f, &side);
    CHECK(out.back() == Catch::Approx(0.75f));
    const auto without = runAlone(*device, 256, {}, 0.5f);
    CHECK(without.back() == Catch::Approx(0.5f));
}

TEST_CASE("A MIDI effect plugin's notes go to the next device", "[hosting][plugins]")
{
    auto device = hostedAlone("Lattice Test Clap Arp");
    CHECK(device->kind() == DeviceKind::MidiEffect);
    std::vector<TimedMidi> out;
    runAlone(*device, 64, {midi(0x90, 60, 100)}, 0.0f, nullptr, &out);
    REQUIRE(out.size() == 2);
    CHECK(out[0].bytes[1] == 60);
    CHECK(out[1].bytes[1] == 72);
}

TEST_CASE("Per-note pitch reaches a CLAP plugin as note expressions, or not at all without MPE", "[hosting][plugins]")
{
    auto device = hostedAlone("Lattice Test Plugin");
    // A member channel's note, and its own bend of +12 semitones (a quarter of 48).
    const auto bend = static_cast<int>(8192 + 8191 / 4);
    const std::vector<TimedMidi> notes{midi(0xe1, static_cast<std::uint8_t>(bend & 127), static_cast<std::uint8_t>(bend >> 7), TimedMidi::perNote),
                                       midi(0x91, 69, 127, TimedMidi::perNote)};
    SECTION("MPE on")
    {
        device->setMpe(true);
        const auto out = runAlone(*device, (1 << 16) + 64, notes);
        CHECK(measureFundamental(out, 880.0).first == Catch::Approx(880.0).margin(1.0));
    }
    SECTION("MPE off: the note, on channel 1, with no bend")
    {
        device->setMpe(false);
        const auto out = runAlone(*device, (1 << 16) + 64, notes);
        CHECK(measureFundamental(out, 440.0).first == Catch::Approx(440.0).margin(0.5));
    }
}

TEST_CASE("A plugin that allocates on the audio thread is counted, not reported as Lattice's", "[hosting][plugins]")
{
    const auto before = rt::violationCount();
    auto device = hostedAlone("Lattice Test Allocator");
    runAlone(*device, 640, {}, 0.5f);
    CHECK(rt::violationCount() == before);
#if !JUCE_WINDOWS
    if (rt::hooksInstalled())
        CHECK(device->allocationsInPlugin() > 0); // its operator new is Lattice's here
#endif
}

TEST_CASE("Another track's compensation fades to a new latency, without a jump", "[hosting][plugins]")
{
    Fixture f(2);
    const auto latency = f.insert(clapPlugin("Lattice Test Latency"), 0); // silent: a MIDI track's input
    f.set().insertDevice(f.chain(1), -1, TestSineDevice::typeId);
    f.load();
    f.render(4800);
    REQUIRE(f.session.processorForTrack(1)->latencySamples() == 0);

    f.renderer.captureAudio = true;
    f.render(4800);
    f.set().setDeviceParameter(latency, "p1", 0.25); // 1000 samples: track 2 now waits 1000, not 64
    for (int i = 0; i < 20; ++i)
    {
        f.render(256);
        f.host.finishLoading();
    }
    f.render(4800);
    REQUIRE(f.session.processorForTrack(0)->latencySamples() == 1000);

    // A 100 Hz sine moves at most 2pi * 100 / 48000 of its level per sample; a jump in its delay
    // would be a step of up to twice its level. The fade keeps every step near the sine's own.
    const auto& out = f.renderer.audioLeft;
    float level = 0.0f, step = 0.0f;
    for (std::size_t i = 1; i < out.size(); ++i)
    {
        level = std::max(level, std::abs(out[i]));
        step = std::max(step, std::abs(out[i] - out[i - 1]));
    }
    REQUIRE(level > 0.1f);
    CHECK(step < 0.03f * level);
}

TEST_CASE("Each device's share of the audio time is measured", "[hosting][plugins]")
{
    Fixture f;
    const auto device = f.insert(clapPlugin("Lattice Test Plugin"));
    f.load();
    addClip(f.set(), 0, 0, 8.0, {NoteData{69, 0.0, 8.0, 127, false}});
    f.session.launchSlot(0, 0);
    auto* hosted = f.live(device);
    REQUIRE(hosted != nullptr);
    hosted->takeCpuLoad(); // from now
    f.render(48000);
    const double load = hosted->takeCpuLoad();
    CHECK(load > 0.0);
    CHECK(load < 1.0);
    CHECK(hosted->takeCpuLoad() < 0.0); // nothing since
}

TEST_CASE("A VST3 plugin's presets save and load as .vstpreset files, as one undo step", "[hosting][plugins]")
{
    Fixture f;
    TempFolder folder;
    const auto device = f.insert(vst3Plugin());
    f.load();
    REQUIRE(f.host.takesVstPresets(device));
    auto* hosted = f.live(device);
    REQUIRE(hosted != nullptr);
    const auto level = hosted->parameter(0).id();

    f.set().setDeviceParameter(device, level, 0.9);
    f.render(256);
    const auto file = folder / "Loud.vstpreset";
    REQUIRE(f.host.saveVstPreset(device, file));
    CHECK(file.getSize() > 0);

    f.set().setDeviceParameter(device, level, 0.1);
    f.render(256);
    f.session.undoManager().clearUndoHistory();
    REQUIRE(f.host.loadVstPreset(device, file));
    CHECK(LiveSet::deviceParameter(device, level, 0.0) == Catch::Approx(0.9).margin(0.001));
    f.render(256);
    CHECK(hosted->plugin().getParameters()[0]->getValue() == Catch::Approx(0.9f).margin(0.001f));
    CHECK(f.session.undoManager().getUndoDescription().startsWith("Load preset Loud"));
    f.session.undoManager().undo();
    CHECK(LiveSet::deviceParameter(device, level, 0.0) == Catch::Approx(0.1).margin(0.001));

    CHECK_FALSE(f.host.loadVstPreset(device, folder / "nothing.vstpreset"));
    const auto clap = f.insert(clapPlugin("Lattice Test Sidechain"));
    f.load();
    CHECK_FALSE(f.host.takesVstPresets(clap));
}
