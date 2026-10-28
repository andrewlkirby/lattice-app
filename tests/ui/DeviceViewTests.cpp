// The Device view and device panels (docs/05-m4-ui.md, 4d), headless: following the selected track,
// return or master; every device type's panel; inserting where you're looking (M3 hands-on finding
// 4), never replacing an instrument silently, hot-swap; the Edit commands on devices; folding and
// "Show all parameters"; racks (macros, chains, zones, map mode, pads); finding controls; the
// Sampler's root from the file (finding 6); edits reaching a rendering engine; and snapshots.

#include "devices/ChainEditing.h"
#include "devices/effects/Compressor.h"
#include "devices/effects/EqEight.h"
#include "devices/instruments/Sampler.h"
#include "devices/racks/Rack.h"
#include "devices/racks/RackEditing.h"
#include "devices/samples/SamplePool.h"
#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"
#include "ui/commands/SessionCommands.h"
#include "ui/device/DeviceCommands.h"
#include "ui/device/DeviceDisplays.h"
#include "ui/device/DevicePanels.h"
#include "ui/device/DeviceView.h"
#include "ui/device/RackMath.h"
#include "ui/device/panels/PanelKit.h"
#include "ui/session/KeyboardFollow.h"
#include "ui/theme/LatticeLookAndFeel.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>

using namespace lattice;
using namespace lattice::ui;
using namespace lattice::test;
using Catch::Approx;

namespace
{
const juce::String synthType = "lattice.synth";
const juce::String samplerType = "lattice.sampler";

/** A mono 16-bit WAV writer at 48 kHz whose smpl chunk names the root note (A4). */
std::unique_ptr<juce::AudioFormatWriter> wavWithRootA4(const juce::File& file)
{
    std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(file);
    juce::WavAudioFormat wav;
    return wav.createWriterFor(out, juce::AudioFormatWriterOptions{}
                                        .withSampleRate(48000.0)
                                        .withNumChannels(1)
                                        .withBitsPerSample(16)
                                        .withMetadataValues({{"MidiUnityNote", "69"}, {"NumSampleLoops", "0"}}));
}

/** A set with two empty MIDI tracks shown in a Device view (the first track's chain). */
struct Fixture
{
    Fixture()
    {
        presetsDir = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("lattice-presets", "");
        library = std::make_unique<presets::PresetLibrary>(presetsDir, session.deviceRegistry());
        session.newSet(2, 1);
        for (int t = 0; t < 2; ++t)
        {
            auto c = chain(t);
            while (c.getNumChildren() > 0)
                set().removeDevice(c.getChild(0));
        }
        session.flush();
        context = std::make_unique<DeviceContext>(DeviceContext{.session = session,
                                                                .feed = feed,
                                                                .specs = specs,
                                                                .presets = *library,
                                                                .tunings = tunings,
                                                                .jobs = jobs});
        context->confirm = [this](const juce::String&, const juce::String& question, const juce::String& yesText,
                                  std::function<void()> yes) {
            questions.add(question);
            if (answerYes && yesText.isNotEmpty() && yes)
                yes();
        };
        view = std::make_unique<DeviceView>(*context, selection);
        view->setLookAndFeel(&laf);
        view->setSize(1600, 320);
        addSessionCommands(commands, session,
                           {[this] { return selection.get(); }, [this](SessionSelection s) { selection.set(s); },
                            [this]() -> EditTarget* { return routeEdits ? view.get() : nullptr; }},
                           {});
        addDeviceCommands(commands, *view);
        view->flushUpdates();
    }
    ~Fixture()
    {
        view->setLookAndFeel(nullptr);
        view = nullptr;
        presetsDir.deleteRecursively();
    }

    LiveSet& set() { return session.set(); }
    juce::ValueTree chain(int track) { return LiveSet::chainOf(set().track(track)); }
    juce::ValueTree insert(const juce::String& type, int track = 0, int index = -1)
    {
        session.undoManager().beginNewTransaction("Insert");
        auto d = set().insertDevice(chain(track), index, type);
        view->flushUpdates();
        return d;
    }
    void flush()
    {
        view->flushUpdates();
        session.flush();
    }
    template <typename T>
    T* live(const juce::ValueTree& device)
    {
        session.flush();
        return dynamic_cast<T*>(session.snapshots().deviceFor(LiveSet::idOf(device)));
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer{engine, BlockPattern::Fixed64};
    EngineStateFeed feed{session, engine};
    DeviceSpecs specs{session.deviceRegistry()};
    juce::File presetsDir;
    std::unique_ptr<presets::PresetLibrary> library;
    JobPool jobs{1};
    SessionSelectionModel selection;
    LatticeLookAndFeel laf;
    CommandRegistry commands{&session.undoManager()};
    std::unique_ptr<DeviceContext> context;
    std::unique_ptr<DeviceView> view;
    juce::StringArray questions;
    bool answerYes = false;
    bool routeEdits = true;
};
} // namespace

// --- What it shows ---------------------------------------------------------------------------------

TEST_CASE("Device view: it shows the selected track's chain, a return's, or the master's", "[ui][device]")
{
    Fixture f;
    f.insert(synthType, 0);
    f.insert("lattice.reverb", 1);
    REQUIRE(f.view->chainView() != nullptr);
    CHECK(f.view->chainView()->chain() == f.chain(0));
    CHECK(f.view->chainView()->frames().size() == 1);
    CHECK(f.view->ownerName() == f.set().track(0)[ids::name].toString());

    f.selection.set({SessionSelection::Kind::slot, 1, 0});
    f.view->flushUpdates();
    CHECK(f.view->chainView()->chain() == f.chain(1));

    REQUIRE(f.set().numReturns() > 0);
    f.selection.showDevicesOf({DeviceOwner::Kind::returnTrack, 0});
    f.view->flushUpdates();
    CHECK(f.view->chainView()->chain() == LiveSet::chainOf(f.set().returnTrack(0)));
    f.selection.showDevicesOf({DeviceOwner::Kind::master, 0});
    f.view->flushUpdates();
    CHECK(f.view->chainView()->chain() == LiveSet::chainOf(f.set().master()));
    CHECK(f.view->ownerName() == "Master");

    // Selecting a scene keeps what's shown; a track shows its own again.
    f.selection.set({SessionSelection::Kind::scene, 0, 0});
    CHECK(f.selection.deviceOwner().kind == DeviceOwner::Kind::master);
    f.selection.set({SessionSelection::Kind::track, 0, 0});
    f.view->flushUpdates();
    CHECK(f.view->chainView()->chain() == f.chain(0));
}

TEST_CASE("Device view: every device type has a body, and Show All Parameters has every parameter", "[ui][device]")
{
    Fixture f;
    for (const auto& type : f.session.deviceRegistry().types())
    {
        if (type.hidden)
            continue;
        INFO(type.id);
        auto c = f.chain(0);
        while (c.getNumChildren() > 0)
            f.set().removeDevice(c.getChild(0));
        const auto device = f.insert(type.id);
        auto* frame = f.view->frameFor(device);
        REQUIRE(frame != nullptr);
        REQUIRE(frame->body() != nullptr);
        CHECK(frame->preferredWidth(300) > 0);

        LiveSet::setDeviceShowsAll(device, true);
        f.flush();
        REQUIRE(frame->body() != nullptr);
        const auto* proto = f.specs.forType(type.id);
        for (int i = 0; i < proto->numParameters(); ++i)
        {
            INFO(proto->parameter(i).id());
            CHECK(frame->body()->controlFor(proto->parameter(i).id()) != nullptr);
        }
    }
}

// --- Inserting (M3 hands-on finding 4) and hot-swap -------------------------------------------------

TEST_CASE("Device view: new devices go where you're looking, including a rack's selected chain", "[ui][device]")
{
    Fixture f;
    const auto synth = f.insert(synthType);
    const auto eq = f.insert("lattice.eq8");
    // The selected device: after it.
    f.view->select(synth);
    REQUIRE(f.view->insertHere(drag::device("lattice.reverb")));
    f.flush();
    CHECK(f.chain(0).getChild(1)[ids::deviceType].toString() == "lattice.reverb");
    CHECK(f.chain(0).getChild(2) == eq);
    CHECK(f.view->selectedDevice() == f.chain(0).getChild(1));

    // A rack's second chain, selected in its chain list: into it.
    f.session.undoManager().beginNewTransaction("Rack");
    const auto rack = racks::insertRack(f.set(), f.chain(0), -1, RackDevice::effectRackId);
    racks::addChain(f.set(), rack, "A");
    const auto second = racks::addChain(f.set(), rack, "B");
    f.flush();
    f.view->selectChain(rack, second);
    f.flush();
    REQUIRE(f.view->insertionChain() == racks::devicesOf(second));
    REQUIRE(f.view->insertHere(drag::device("lattice.delay")));
    f.flush();
    CHECK(racks::devicesOf(second).getNumChildren() == 1);
    CHECK(racks::devicesOf(second).getChild(0)[ids::deviceType].toString() == "lattice.delay");
    CHECK(racks::devicesOf(rack.getChildWithName(ids::rackChains).getChild(0)).getNumChildren() == 0);
    // The Device menu's Insert commands go to the same place.
    REQUIRE(f.commands.perform("device.insert.chorus"));
    f.flush();
    CHECK(racks::devicesOf(second).getChild(1)[ids::deviceType].toString() == "lattice.chorus");

    // Clicking the top chain takes inserts back to it.
    f.view->focusChain(f.chain(0));
    CHECK(f.view->insertionChain() == f.chain(0));
}

TEST_CASE("Device view: an instrument never replaces another silently; hot-swap replaces at once", "[ui][device]")
{
    Fixture f;
    const auto synth = f.insert(synthType);
    f.insert("lattice.reverb");

    // Asked, and answered no: nothing changes.
    CHECK_FALSE(f.view->insertHere(drag::device(samplerType)));
    REQUIRE(f.questions.size() == 1);
    CHECK(f.questions[0].contains("Synth"));
    CHECK(f.chain(0).getChild(0) == synth);

    // Answered yes: replaced, as one undo step.
    f.answerYes = true;
    f.view->insertHere(drag::device(samplerType));
    f.flush();
    CHECK(f.questions.size() == 2);
    CHECK(f.chain(0).getChild(0)[ids::deviceType].toString() == samplerType);
    CHECK(f.chain(0).getNumChildren() == 2);
    f.session.undoManager().undo();
    f.flush();
    CHECK(f.chain(0).getChild(0)[ids::deviceType].toString() == synthType);

    // Hot-swap: no question, the target is replaced in its place, and the mode ends.
    f.answerYes = false;
    const auto reverb = f.chain(0).getChild(1);
    f.view->setHotSwapTarget(reverb);
    REQUIRE(f.view->insertHere(drag::device("lattice.delay")));
    f.flush();
    CHECK(f.questions.size() == 2);
    CHECK(f.chain(0).getChild(1)[ids::deviceType].toString() == "lattice.delay");
    CHECK(f.chain(0).getNumChildren() == 2);
    CHECK_FALSE(f.view->hotSwapTarget().isValid());
    f.view->setHotSwapTarget(f.chain(0).getChild(0));
    REQUIRE(f.view->insertHere(drag::device(samplerType)));
    f.flush();
    CHECK(f.questions.size() == 2);
    CHECK(f.chain(0).getChild(0)[ids::deviceType].toString() == samplerType);
}

TEST_CASE("Device view: Duplicate, Delete, Copy, Paste and Rename act on the selected device", "[ui][device]")
{
    Fixture f;
    const auto eq = f.insert("lattice.eq8");
    f.view->select(eq);
    REQUIRE(f.commands.perform("edit.duplicate"));
    f.flush();
    REQUIRE(f.chain(0).getNumChildren() == 2);
    CHECK(f.view->selectedDevice() == f.chain(0).getChild(1));
    f.session.undoManager().undo();
    f.flush();
    CHECK(f.chain(0).getNumChildren() == 1);

    f.view->select(eq);
    REQUIRE(f.commands.perform("edit.copy"));
    f.view->select({});
    f.view->focusChain(f.chain(1));
    f.selection.set({SessionSelection::Kind::track, 1, 0});
    f.flush();
    REQUIRE(f.commands.perform("edit.paste"));
    f.flush();
    CHECK(f.chain(1).getNumChildren() == 1);
    CHECK(f.chain(1).getChild(0)[ids::deviceType].toString() == "lattice.eq8");
    CHECK(LiveSet::idOf(f.chain(1).getChild(0)) != LiveSet::idOf(eq));

    f.view->select(f.chain(1).getChild(0));
    REQUIRE(f.commands.perform("edit.delete"));
    f.flush();
    CHECK(f.chain(1).getNumChildren() == 0);

    // Rename: the device's own name, shown in its title; empty goes back to the type's.
    f.selection.set({SessionSelection::Kind::track, 0, 0});
    f.flush();
    f.view->select(eq);
    CHECK(f.view->canEdit("edit.rename"));
    f.set().setDeviceName(eq, "Bright");
    f.flush();
    CHECK(f.context->nameOf(eq) == "Bright");
    CHECK(f.view->frameFor(eq)->getTitle() == "Bright");
    f.set().setDeviceName(eq, "");
    CHECK(f.context->nameOf(eq) == "EQ Eight");
}

TEST_CASE("Device view: folding and Show All Parameters are saved, not undone, and don't rebuild the engine", "[ui][device]")
{
    Fixture f;
    const auto synth = f.insert(synthType);
    f.session.flush();
    auto* before = f.session.snapshots().deviceFor(LiveSet::idOf(synth));
    REQUIRE(before != nullptr);
    auto* frame = f.view->frameFor(synth);
    const int unfolded = frame->preferredWidth(300);
    f.session.undoManager().beginNewTransaction("Fold");
    frame->foldButton().onClick();
    f.flush();
    CHECK(frame->isFolded());
    CHECK(frame->preferredWidth(300) < unfolded / 5);
    frame->allParametersButton().onClick();
    f.flush();
    CHECK(frame->showsAllParameters());
    CHECK(f.session.undoManager().getNumActionsInCurrentTransaction() == 0);
    CHECK(f.session.hasUnsavedChanges());
    CHECK(f.session.snapshots().deviceFor(LiveSet::idOf(synth)) == before);
    // The commands do the same to the selected device.
    f.view->select(synth);
    REQUIRE(f.commands.perform("device.fold"));
    f.flush();
    CHECK_FALSE(frame->isFolded());
}

// --- Panels ------------------------------------------------------------------------------------------------

TEST_CASE("Device panels: each custom panel reveals any of its parameters (pages, bands, modes)", "[ui][device]")
{
    Fixture f;
    for (const auto* type : {"lattice.synth", "lattice.sampler", "lattice.eq8", "lattice.compressor", "lattice.reverb", "lattice.delay",
                             "lattice.autofilter", "lattice.saturator", "lattice.chorus", "lattice.utility", "lattice.limiter",
                             "lattice.arpeggiator", "lattice.chord"})
    {
        INFO(type);
        auto c = f.chain(0);
        while (c.getNumChildren() > 0)
            f.set().removeDevice(c.getChild(0));
        const auto device = f.insert(type);
        auto* frame = f.view->frameFor(device);
        REQUIRE(frame != nullptr);
        const auto* proto = f.specs.forType(type);
        int own = 0;
        for (int i = 0; i < proto->numParameters(); ++i)
        {
            const auto id = proto->parameter(i).id();
            INFO(id);
            auto* control = f.view->revealParameter(device, id);
            CHECK(control != nullptr);
            if (!frame->showsAllParameters())
                ++own;
            LiveSet::setDeviceShowsAll(device, false);
            f.flush();
        }
        // Every parameter is on the device's own panel: Show All is never needed.
        CHECK(own == proto->numParameters());
    }
}

TEST_CASE("Device panels: the EQ's handles move a band, one undo step per drag", "[ui][device]")
{
    Fixture f;
    const auto eq = f.insert("lattice.eq8");
    auto* body = f.view->frameFor(eq)->body();
    REQUIRE(body != nullptr);
    juce::Component* curve = nullptr;
    for (auto* c : body->getChildren())
        if (c->getTitle() == "EQ curve")
            curve = c;
    REQUIRE(curve != nullptr);
    curve->setSize(400, 200);
    // Band 3 (a bell at 250 Hz, 0 dB): drag it up and to the right.
    const auto area = curve->getLocalBounds().toFloat().reduced(themeOf(*curve).stroke + 1.0f);
    const juce::Point<float> from{FrequencyAxis::xFor(250.0, area), yForDb(0.0f, area, -15.0f, 15.0f)};
    const juce::Point<float> to{FrequencyAxis::xFor(1000.0, area), yForDb(6.0f, area, -15.0f, 15.0f)};
    const auto mouse = juce::Desktop::getInstance().getMainMouseSource();
    const auto event = [&](juce::Point<float> p) {
        return juce::MouseEvent(mouse, p, {}, juce::MouseInputSource::defaultPressure, 0.0f, 0.0f, 0.0f, 0.0f, curve, curve,
                                juce::Time::getCurrentTime(), from, juce::Time::getCurrentTime(), 1, false);
    };
    curve->mouseDown(event(from));
    curve->mouseDrag(event({(from.x + to.x) / 2, (from.y + to.y) / 2}));
    curve->mouseDrag(event(to));
    curve->mouseUp(event(to));
    CHECK(LiveSet::deviceParameter(eq, "band3.frequency", 0.0) == Approx(1000.0).epsilon(0.03));
    CHECK(LiveSet::deviceParameter(eq, "band3.gain", 0.0) == Approx(6.0).margin(0.3));
    f.session.undoManager().undo();
    CHECK(LiveSet::deviceParameter(eq, "band3.frequency", 250.0) == Approx(250.0));
    CHECK(LiveSet::deviceParameter(eq, "band3.gain", 0.0) == Approx(0.0));
}

TEST_CASE("Device panels: the sidechain picker says why a track isn't offered", "[ui][device]")
{
    Fixture f;
    f.set().addTrack(-1); // a third track
    const auto comp1 = f.insert("lattice.compressor", 0);
    const auto comp2 = f.insert("lattice.compressor", 1);
    f.session.undoManager().beginNewTransaction("Sidechain");
    REQUIRE(chains::setSidechainSource(f.set(), comp1, f.set().trackId(1), false));
    f.selection.set({SessionSelection::Kind::track, 1, 0});
    f.flush();
    panels::SidechainPicker picker(*f.context, comp2);
    CHECK(picker.whyNot(1) == "its own track");
    CHECK(picker.whyNot(0).contains("feed back")); // track 1 already hears track 2
    CHECK(picker.whyNot(2).isEmpty());
    // The menu lists them all; the ones not offered can't be chosen.
    const auto menu = picker.sourceMenu();
    int enabled = 0, items = 0;
    for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
        if (!it.getItem().isSeparator)
        {
            ++items;
            enabled += it.getItem().isEnabled ? 1 : 0;
        }
    CHECK(items == 1 + f.set().numTracks());
    CHECK(enabled == 2); // no source, and track 3
}

TEST_CASE("Device panels: a sample's root note from the file sets the Sampler's root, in the same undo step", "[ui][device]")
{
    // M3 hands-on finding 6: a 440 Hz sample whose file says A4 plays at 440 Hz without typing.
    Fixture f;
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("a440", ".wav");
    {
        auto writer = wavWithRootA4(file);
        REQUIRE(writer != nullptr);
        juce::AudioBuffer<float> tone(1, 24000);
        for (int i = 0; i < tone.getNumSamples(); ++i)
            tone.setSample(0, i, 0.5f * std::sin(2.0f * juce::MathConstants<float>::pi * 440.0f * static_cast<float>(i) / 48000.0f));
        writer->writeFromAudioSampleBuffer(tone, 0, tone.getNumSamples());
    }
    const auto sampler = f.insert(samplerType);
    REQUIRE(LiveSet::deviceParameter(sampler, "sample.root", 261.6256) == Approx(261.6256).margin(0.01));
    auto* frame = f.view->frameFor(sampler);
    frame->filesDropped({file.getFullPathName()}, 10, 10);
    SamplePool::shared().waitUntilIdle();
    SamplePool::shared().deliverFinished();
    CHECK(LiveSet::deviceParameter(sampler, "sample.root", 0.0) == Approx(440.0).margin(0.01));
    CHECK(sampler.getChildWithName(ids::deviceState)["file"].toString() == file.getFullPathName());
    f.session.undoManager().undo();
    CHECK(LiveSet::deviceParameter(sampler, "sample.root", 261.6256) == Approx(261.6256).margin(0.01));
    CHECK(sampler.getChildWithName(ids::deviceState)["file"].toString().isEmpty());

    // A drum pad keeps its root: it plays the sample at its own pitch.
    f.session.undoManager().beginNewTransaction("Drums");
    const auto drums = racks::insertRack(f.set(), f.chain(1), -1, RackDevice::drumRackId);
    const auto pad = racks::addDrumPad(f.set(), drums, 36);
    const auto padSampler = racks::devicesOf(pad).getChild(0);
    panels::loadSampleInto(*f.context, padSampler, file);
    SamplePool::shared().waitUntilIdle();
    SamplePool::shared().deliverFinished();
    CHECK(LiveSet::deviceParameter(padSampler, "sample.root", 261.6256) == Approx(261.6256).margin(0.01));
    file.deleteFile();
}

// --- Racks -------------------------------------------------------------------------------------------------

TEST_CASE("Rack panels: macros fit in rows; zones drag within range; pads page with the space", "[ui][device][racks]")
{
    using namespace rackmath;
    CHECK(fitMacros(8, 150, 70).rows == 2);
    CHECK(fitMacros(8, 150, 70).columns == 4);
    CHECK(fitMacros(64, 150, 70).columns == 32);
    CHECK(fitMacros(64, 700, 70).rows == 10);
    CHECK(fitMacros(64, 700, 70).columns == 7);
    CHECK(fitMacros(3, 10, 70).rows == 1); // never fewer than one row
    CHECK(fitMacros(0, 300, 70).columns == 0);

    const Zone z{36, 60, 4, 2};
    CHECK(dragZone(z, ZoneHandle::low, -10) == Zone{26, 60, 4, 2});
    CHECK(dragZone(z, ZoneHandle::low, 100).low == 60); // not past the other end
    CHECK(dragZone(z, ZoneHandle::low, 100).fadeLow == 0);
    CHECK(dragZone(z, ZoneHandle::high, 100).high == 127);
    CHECK(dragZone(z, ZoneHandle::fadeLow, 3).fadeLow == 7);
    CHECK(dragZone(z, ZoneHandle::fadeLow, 100).fadeLow == 22); // the fades stay inside the zone
    CHECK(dragZone(z, ZoneHandle::fadeHigh, -5).fadeHigh == 7);
    CHECK(dragZone(z, ZoneHandle::body, 100) == Zone{103, 127, 4, 2});
    CHECK(dragZone(z, ZoneHandle::body, -100) == Zone{0, 24, 4, 2});
    CHECK(dragZone(Zone{1, 127, 0, 0}, ZoneHandle::low, -10, 1).low == 1); // velocity starts at 1
    // Handles: the ends, the fade handles along the top, the body.
    const float left = 0.0f, width = 128.0f; // a pixel per note
    CHECK(zoneHandleAt(z, 36.0f, 15.0f, left, width, 0.0f, 20.0f, 2.0f) == ZoneHandle::low);
    CHECK(zoneHandleAt(z, 61.0f, 15.0f, left, width, 0.0f, 20.0f, 2.0f) == ZoneHandle::high);
    CHECK(zoneHandleAt(z, 40.0f, 3.0f, left, width, 0.0f, 20.0f, 2.0f) == ZoneHandle::fadeLow);
    CHECK(zoneHandleAt(z, 59.0f, 3.0f, left, width, 0.0f, 20.0f, 2.0f) == ZoneHandle::fadeHigh);
    CHECK(zoneHandleAt(z, 48.0f, 15.0f, left, width, 0.0f, 20.0f, 2.0f) == ZoneHandle::body);
    CHECK(zoneHandleAt(z, 90.0f, 15.0f, left, width, 0.0f, 20.0f, 2.0f) == ZoneHandle::none);

    CHECK(fitPads(200, 200, 44, 0).size() == 16);
    CHECK(fitPads(200, 400, 44, 0).rows == 8);
    CHECK(fitPads(200, 400, 44, 0).columns == 4);
    CHECK(fitPads(400, 400, 44, 0).size() == 64);
    CHECK(fitPads(100, 100, 44, 32).size() == 32);
}

TEST_CASE("Rack panels: map mode maps a click on a parameter, the macro drives it in the engine", "[ui][device][racks]")
{
    Fixture f;
    f.session.undoManager().beginNewTransaction("Rack");
    const auto rack = racks::insertRack(f.set(), f.chain(0), -1, RackDevice::instrumentRackId);
    const auto chain = racks::addChain(f.set(), rack);
    const auto synth = f.set().insertDevice(racks::devicesOf(chain), -1, synthType);
    f.flush();
    auto* synthFrame = f.view->frameFor(synth);
    REQUIRE(synthFrame != nullptr);
    CHECK_FALSE(synthFrame->isMapping());

    f.view->setMapMode(rack, 1);
    f.flush();
    CHECK(synthFrame->isMapping());
    CHECK_FALSE(f.view->frameFor(rack)->isMapping()); // the rack's own macros stay macros
    REQUIRE(f.view->mapTo(synth, "filter.cutoff"));
    REQUIRE(racks::mappingsOf(rack, 1).size() == 1);
    CHECK(racks::mappingsOf(rack, 1)[0][ids::parameter].toString() == "filter.cutoff");
    // Again: unmapped.
    f.view->mapTo(synth, "filter.cutoff");
    CHECK(racks::mappingsOf(rack, 1).empty());
    f.view->mapTo(synth, "filter.cutoff");
    f.view->setMapMode({}, -1);
    f.flush();
    CHECK_FALSE(synthFrame->isMapping());

    // The macro turns the cutoff in the engine: at 0 the bottom of its range, at 127 the top.
    f.set().setDeviceParameter(rack, "macro2", 127.0);
    f.session.flush();
    f.renderer.renderFor(512);
    auto* live = f.session.snapshots().deviceFor(LiveSet::idOf(synth));
    REQUIRE(live != nullptr);
    CHECK(live->findParameter("filter.cutoff")->plain() == Approx(20000.0f).epsilon(0.01));
    f.set().setDeviceParameter(rack, "macro2", 0.0);
    f.session.flush();
    f.renderer.renderFor(512);
    CHECK(live->findParameter("filter.cutoff")->plain() == Approx(20.0f).epsilon(0.01));

    // A narrower range, edited as the mapping list does, reaches the engine too.
    f.session.undoManager().beginNewTransaction("Range");
    racks::setMappingRange(f.set(), racks::mappingsOf(rack, 1)[0], 0.5, 0.5);
    f.session.flush();
    f.renderer.renderFor(512);
    live = f.session.snapshots().deviceFor(LiveSet::idOf(synth));
    const auto& spec = live->findParameter("filter.cutoff")->spec();
    CHECK(live->findParameter("filter.cutoff")->plain() == Approx(spec.normalisableRange().convertFrom0to1(0.5f)).epsilon(0.01));
}

TEST_CASE("Rack panels: revealing a parameter opens the chain it's in, unfolds and flips to its page", "[ui][device][racks]")
{
    Fixture f;
    f.session.undoManager().beginNewTransaction("Rack");
    const auto rack = racks::insertRack(f.set(), f.chain(1), -1, RackDevice::instrumentRackId);
    racks::addChain(f.set(), rack, "A");
    const auto b = racks::addChain(f.set(), rack, "B");
    const auto synth = f.set().insertDevice(racks::devicesOf(b), -1, synthType);
    LiveSet::setDeviceFolded(rack, true);
    f.flush();
    CHECK(f.view->frameFor(synth) == nullptr); // another track's

    bool shown = false;
    f.view->onShowRequested = [&shown] { shown = true; };
    auto* control = f.view->revealParameter(synth, "lfo2.rate");
    REQUIRE(control != nullptr);
    CHECK(shown);
    CHECK(f.selection.get().track == 1);
    CHECK(f.view->selectedChainOf(rack) == b);
    CHECK_FALSE(static_cast<bool>(rack.getProperty(ids::folded, false)));
    CHECK(f.view->frameFor(synth) != nullptr);
    CHECK(f.view->isParentOf(control));
    CHECK(f.view->flashedControl() == control);
    CHECK(parameterIdOf(*control) == "lfo2.rate");
}

TEST_CASE("Rack panels: controls tell the registry where they live and how to bring them back", "[ui][device]")
{
    struct Host final : juce::Component, ControlRegistry::Provider
    {
        ControlRegistry& controlRegistry() override { return registry; }
        ControlRegistry registry;
    };
    Host host; // first, so its registry outlives the view's controls
    Fixture f;
    host.setSize(1600, 320);
    host.addAndMakeVisible(*f.view);
    const auto synth = f.insert(synthType);
    f.flush();
    // The Synth opens on its oscillators' page: those controls are on screen.
    const auto found = host.registry.search("pulse width");
    REQUIRE_FALSE(found.empty());
    const auto* entry = found.front();
    CHECK(entry->owner == "Synth" + juce::String::fromUTF8(" \xc2\xb7 ") + f.set().track(0)[ids::name].toString());
    CHECK(entry->group.startsWith("Osc "));
    REQUIRE(entry->reveal != nullptr);
    LiveSet::setDeviceFolded(synth, true);
    f.flush();
    entry->reveal();
    CHECK_FALSE(static_cast<bool>(synth.getProperty(ids::folded, false)));
    host.removeChildComponent(f.view.get());
}

TEST_CASE("Rack panels: pads take samples, move and swap, and the keyboard's page follows the grid", "[ui][device][racks]")
{
    Fixture f;
    f.session.undoManager().beginNewTransaction("Drums");
    auto rack = racks::insertRack(f.set(), f.chain(0), -1, RackDevice::drumRackId);
    auto kick = racks::addDrumPad(f.set(), rack, 36);
    kick.setProperty(ids::name, "Kick", nullptr);
    const auto snare = racks::addDrumPad(f.set(), rack, 38);
    f.flush();
    CHECK(drumPadPageStart(f.set(), 0) == 36);

    f.session.undoManager().beginNewTransaction("Move");
    REQUIRE(racks::movePad(f.set(), rack, 36, 38));
    CHECK(static_cast<int>(kick[ids::receiveNote]) == 38);
    CHECK(static_cast<int>(snare[ids::receiveNote]) == 36);
    f.session.undoManager().undo();
    CHECK(static_cast<int>(kick[ids::receiveNote]) == 36);

    // Scrolling the grid moves the page the computer keyboard plays.
    rack.setProperty(ids::padStart, 40, nullptr);
    CHECK(drumPadPageStart(f.set(), 0) == 40);

    // Choke and tune per pad, as the pad list edits them, one undo step each.
    f.session.undoManager().beginNewTransaction("Choke");
    racks::setChokeGroup(f.set(), kick, 2);
    racks::setPadPlayNote(f.set(), kick, 62);
    CHECK(static_cast<int>(kick[ids::chokeGroup]) == 2);
    CHECK(static_cast<int>(kick[ids::playNote]) == 62);
}

// --- The feed and the engine ----------------------------------------------------------------------------

TEST_CASE("Device view: the EQ's spectrum and the compressor's gain reduction come from the engine", "[ui][device]")
{
    Fixture f;
    f.insert(TestSineDevice::typeId);
    const auto eq = f.insert("lattice.eq8");
    const auto comp = f.insert("lattice.compressor");
    f.set().setDeviceParameter(comp, "comp.threshold", -40.0);
    f.session.flush();
    f.feed.watchDevice(LiveSet::idOf(eq));
    f.feed.watchDevice(LiveSet::idOf(comp));
    for (int i = 0; i < 20; ++i)
    {
        f.renderer.renderFor(800);
        f.feed.tick(1.0 + i / 60.0);
    }
    const auto* spectrum = f.feed.device(LiveSet::idOf(eq));
    REQUIRE(spectrum != nullptr);
    REQUIRE_FALSE(spectrum->spectrumDb.empty());
    // The test sine is at 100 Hz: its bin is the loudest.
    const auto loudest = std::max_element(spectrum->spectrumDb.begin(), spectrum->spectrumDb.end()) - spectrum->spectrumDb.begin();
    const double binHz = spectrum->sampleRate / EngineStateFeed::spectrumSize;
    CHECK(static_cast<double>(loudest) * binHz == Approx(100.0).margin(2.0 * binHz));
    const auto* reduction = f.feed.device(LiveSet::idOf(comp));
    REQUIRE(reduction != nullptr);
    CHECK(reduction->gainReductionDb < -6.0f);
    f.feed.unwatchDevice(LiveSet::idOf(eq));
    f.feed.unwatchDevice(LiveSet::idOf(comp));
    // Its panel still watches it, until the device goes.
    CHECK(f.feed.device(LiveSet::idOf(eq)) != nullptr);
    f.set().removeDevice(eq);
    f.flush();
    CHECK(f.feed.device(LiveSet::idOf(eq)) == nullptr);
}

TEST_CASE("Device view: devices inserted and turned while the engine plays are heard", "[ui][device]")
{
    // Tests, item 12: edits made while rendering reach the sound.
    Fixture f;
    f.insert(TestSineDevice::typeId);
    f.session.flush();
    f.renderer.captureAudio = true;
    const auto level = [&f] {
        f.renderer.audioLeft.clear();
        f.renderer.renderFor(4800);
        float peak = 0.0f;
        for (std::size_t i = 2400; i < f.renderer.audioLeft.size(); ++i)
            peak = std::max(peak, std::abs(f.renderer.audioLeft[i]));
        return peak;
    };
    const float before = level();
    REQUIRE(before > 0.05f);
    f.view->focusChain(f.chain(0));
    REQUIRE(f.view->insertHere(drag::device("lattice.utility")));
    f.session.flush();
    const auto utility = f.chain(0).getChild(1);
    auto* knob = dynamic_cast<BoundControl*>(f.view->frameFor(utility)->body()->controlFor("utility.gain"));
    REQUIRE(knob != nullptr);
    knob->binding().setValue(-24.0f);
    const float after = level();
    CHECK(after == Approx(before * std::pow(10.0f, -24.0f / 20.0f)).epsilon(0.1));
    // Off: the sound comes back.
    f.view->frameFor(utility)->powerButton().onClick();
    CHECK(level() == Approx(before).epsilon(0.05));
}

TEST_CASE("Device view timing: a rack with 64 macros paints within a frame while they move", "[ui][device][timing]")
{
    // Tests, item 10: the Device view showing a rack with 64 macros (and a Synth in its chain).
    Fixture f;
    f.view->setSize(1600, 360);
    f.session.undoManager().beginNewTransaction("Rack");
    const auto rack = racks::insertRack(f.set(), f.chain(0), -1, RackDevice::instrumentRackId);
    racks::setMacroCount(f.set(), rack, 64);
    const auto chain = racks::addChain(f.set(), rack);
    f.set().insertDevice(racks::devicesOf(chain), -1, synthType);
    f.flush();
    juce::Image target(juce::Image::ARGB, f.view->getWidth(), f.view->getHeight(), true);
    std::vector<double> times;
    for (int frame = 0; frame < 60; ++frame)
    {
        // Every frame turns a few macros, as a controller would.
        for (int m = 0; m < 4; ++m)
            f.set().setDeviceParameter(rack, "macro" + juce::String((frame * 4 + m) % 64 + 1), static_cast<double>(frame * 2 % 128));
        juce::Graphics g(target);
        const auto start = juce::Time::getHighResolutionTicks();
        f.view->paintEntireComponent(g, true);
        times.push_back(juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - start));
    }
    std::sort(times.begin(), times.end());
    const double median = times[times.size() / 2];
    WARN("Device view with a 64-macro rack: median frame " << median * 1000.0 << " ms, worst " << times.back() * 1000.0 << " ms");
    CHECK(median < 0.008);
}

// --- Snapshots ---------------------------------------------------------------------------------------

namespace
{
void writeSnapshot(juce::Component& c, const juce::String& name, float scale = 1.0f)
{
    // lattice_tests "[.ui-snapshot]" writes PNGs to LATTICE_SNAPSHOT_DIR (default: temp).
    const juce::File dir(juce::SystemStats::getEnvironmentVariable(
        "LATTICE_SNAPSHOT_DIR", juce::File::getSpecialLocation(juce::File::tempDirectory).getFullPathName()));
    const auto image = c.createComponentSnapshot(c.getLocalBounds(), true, scale);
    const auto file = dir.getChildFile(name + ".png");
    file.deleteFile();
    juce::FileOutputStream out(file);
    juce::PNGImageFormat png;
    REQUIRE(png.writeImageToStream(image, out));
}
} // namespace

TEST_CASE("Device view snapshot: every panel, in every theme at 100% and 200%", "[.ui-snapshot]")
{
    const auto tone = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lattice-snapshot-tone.wav");
    {
        tone.deleteFile();
        auto writer = wavWithRootA4(tone);
        REQUIRE(writer != nullptr);
        juce::AudioBuffer<float> b(1, 48000);
        for (int i = 0; i < b.getNumSamples(); ++i) // a decaying 440 Hz with a second hit halfway
            b.setSample(0, i, 0.8f * std::exp(-4.0f * static_cast<float>(i % 24000) / 24000.0f) *
                                  std::sin(2.0f * juce::MathConstants<float>::pi * 440.0f * static_cast<float>(i) / 48000.0f));
        writer->writeFromAudioSampleBuffer(b, 0, b.getNumSamples());
    }
    struct Shot
    {
        const char* name;
        std::function<void(Fixture&)> build;
    };
    const std::vector<Shot> shots{
        {"instruments", [&tone](Fixture& f) {
             f.insert("lattice.chord");
             f.set().setDeviceParameter(f.chain(0).getChild(0), "chord.shift1", 4.0);
             f.set().setDeviceParameter(f.chain(0).getChild(0), "chord.shift2", 7.0);
             f.insert("lattice.arpeggiator");
             f.insert(synthType);
             const auto sampler = f.insert(samplerType, 1);
             panels::loadSampleInto(*f.context, sampler, tone);
             f.set().setDeviceParameter(sampler, "loop.on", 1.0);
             f.set().setDeviceParameter(sampler, "loop.start", 0.3);
             f.set().setDeviceParameter(sampler, "loop.length", 0.4);
         }},
        {"synth-pages", [](Fixture& f) {
             const auto a = f.insert(synthType);
             const auto b = f.insert(synthType);
             f.view->revealParameter(a, "filter.cutoff");
             f.view->revealParameter(b, "mod1.amount");
             f.set().setDeviceParameter(a, "filter.resonance", 0.6);
         }},
        {"sampler", [&tone](Fixture& f) {
             const auto sampler = f.insert(samplerType);
             panels::loadSampleInto(*f.context, sampler, tone);
             f.set().setDeviceParameter(sampler, "sample.mode", 2.0);
             f.set().setDeviceParameter(sampler, "slice.by", 2.0);
             sampler.getChildWithName(ids::deviceState).setProperty("slices", "6000,24000,30000", nullptr);
         }},
        {"effects-1", [](Fixture& f) {
             const auto eq = f.insert("lattice.eq8");
             f.set().setDeviceParameter(eq, "band3.gain", 6.0);
             f.set().setDeviceParameter(eq, "band6.gain", -4.0);
             f.set().setDeviceParameter(eq, "band6.target", 2.0);
             const auto comp = f.insert("lattice.compressor");
             f.set().setDeviceParameter(comp, "comp.threshold", -24.0);
             f.set().setDeviceParameter(comp, "comp.knee", 6.0);
             f.insert("lattice.reverb");
         }},
        {"effects-2", [](Fixture& f) {
             f.insert("lattice.delay");
             f.insert("lattice.autofilter");
             f.insert("lattice.saturator");
         }},
        {"effects-3", [](Fixture& f) {
             f.insert("lattice.chorus");
             f.insert("lattice.utility");
             f.insert("lattice.limiter");
             f.insert(synthType, 1);
             LiveSet::setDeviceFolded(f.chain(0).getChild(0), true);
         }},
        {"racks", [](Fixture& f) {
             f.session.undoManager().beginNewTransaction("Rack");
             const auto rack = racks::insertRack(f.set(), f.chain(0), -1, RackDevice::instrumentRackId);
             racks::setMacroCount(f.set(), rack, 16);
             racks::setMacroName(f.set(), rack, 0, "Brightness");
             racks::setMacroColour(f.set(), rack, 1, LiveSet::paletteColour(3));
             const auto a = racks::addChain(f.set(), rack, "Low");
             const auto b = racks::addChain(f.set(), rack, "High");
             racks::setKeyZone(f.set(), a, 0, 59, 0, 6);
             racks::setKeyZone(f.set(), b, 54, 127, 6, 0);
             racks::setChainMix(f.set(), b, -6.0, 0.3, false, true);
             const auto synth = f.set().insertDevice(racks::devicesOf(a), -1, synthType);
             racks::addMapping(f.set(), rack, 0, synth, "filter.cutoff", 0.2, 0.9);
             f.flush();
             f.view->setMapMode(rack, 0);
             if (auto* frame = f.view->frameFor(rack))
                 if (auto* zonesButton = dynamic_cast<juce::Button*>([&]() -> juce::Component* {
                         std::function<juce::Component*(juce::Component&)> find = [&](juce::Component& c) -> juce::Component* {
                             for (auto* child : c.getChildren())
                             {
                                 if (auto* button = dynamic_cast<juce::TextButton*>(child); button != nullptr && button->getButtonText() == "Zones")
                                     return button;
                                 if (auto* found = find(*child))
                                     return found;
                             }
                             return nullptr;
                         };
                         return find(*frame);
                     }()))
                 {
                     zonesButton->setToggleState(true, juce::dontSendNotification);
                     zonesButton->onClick();
                 }
         }},
        {"drums", [&tone](Fixture& f) {
             f.session.undoManager().beginNewTransaction("Drums");
             auto rack = racks::insertRack(f.set(), f.chain(0), -1, RackDevice::drumRackId);
             for (const auto& [note, name] : {std::pair{36, "Kick"}, std::pair{38, "Snare"}, std::pair{42, "Hat"}, std::pair{46, "Open Hat"}})
             {
                 auto pad = racks::addDrumPad(f.set(), rack, note, tone.getFullPathName());
                 pad.setProperty(ids::name, name, nullptr);
             }
             racks::setChokeGroup(f.set(), racks::padFor(rack, 42), 1);
             racks::setChokeGroup(f.set(), racks::padFor(rack, 46), 1);
             f.view->selectChain(rack, racks::padFor(rack, 38));
         }},
    };
    for (const auto& shot : shots)
    {
        Fixture f;
        f.view->setSize(2400, 300);
        shot.build(f);
        f.flush();
        SamplePool::shared().waitUntilIdle();
        SamplePool::shared().deliverFinished();
        f.jobs.waitUntilIdle(2000);
        f.jobs.deliverFinished();
        f.flush();
        for (const auto* theme : {&Theme::dark(), &Theme::light(), &Theme::highContrast()})
        {
            f.laf.setTheme(*theme);
            f.view->sendLookAndFeelChange();
            f.flush();
            for (const float scale : {1.0f, 2.0f})
                if (scale == 1.0f || theme == &Theme::dark())
                    writeSnapshot(*f.view, "device-" + juce::String(shot.name) + "-" + theme->name.removeCharacters(" ").toLowerCase() + "-" +
                                               juce::String(juce::roundToInt(scale * 100)),
                                  scale);
        }
    }
    tone.deleteFile();
}
