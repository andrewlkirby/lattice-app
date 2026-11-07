// Device and Mix modes and the encoders (docs/06-m5-controllers.md, 5c; Tests 1, "Device" and
// "Mix"): the pages equal ParameterPager's for 8, 16 and 32 encoders, a rack's macros, a turn as
// one undo step, fine turns and detents, absolute controls picking up, choosing devices and going
// into racks, the On switch and the pages, Mix's volumes, pans, sends and track page, the grid as
// faders on a device without encoders, the two modes running together, the display frames, text
// displays, and every candidate device.

#include "support/FakeSurface.h"
#include "support/TestDevices.h"
#include "control/modes/DeviceMode.h"
#include "control/modes/MixMode.h"
#include "control/modes/SessionMode.h"
#include "control/surface/Presets.h"
#include "control/surface/SurfaceController.h"
#include "devices/effects/EqEight.h"
#include "devices/framework/ParameterPager.h"
#include "devices/instruments/Synth.h"
#include "devices/racks/Rack.h"
#include "devices/racks/RackEditing.h"
#include "ui/commands/SessionCommands.h"
#include "ui/mapping/Targets.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace lattice;
using namespace lattice::control;
using namespace lattice::test;
using Catch::Matchers::WithinAbs;
using Kind = ui::SessionSelection::Kind;

namespace
{
struct Fixture
{
    explicit Fixture(SurfaceCapabilities caps = presets::push2(), int tracks = 4) : surface(std::move(caps))
    {
        session.newSet(tracks, 2);
        for (int t = 0; t < tracks; ++t)
        {
            auto c = chain(t);
            while (c.getNumChildren() > 0)
                session.set().removeDevice(c.getChild(0));
            session.set().setColour(session.set().track(t), 0xff000000u | static_cast<juce::uint32>(0x204080 + t * 0x101010));
        }
        session.flush();
        ui::addSessionCommands(commands, session, {[this] { return selection.get(); }, [this](ui::SessionSelection s) { selection.set(s); }}, {});
        controller = std::make_unique<SurfaceController>(SurfaceContext{session, commands, selection, feed, &specs}, surface);
        selection.set({Kind::track, 0, 0});
        refresh();
    }
    ~Fixture() { controller.reset(); }

    juce::ValueTree chain(int t) { return LiveSet::chainOf(session.set().track(t)); }
    juce::ValueTree add(int t, const char* type)
    {
        auto d = session.set().insertDevice(chain(t), -1, type);
        session.flush();
        refresh();
        return d;
    }
    void refresh() { controller->refresh(); }
    DeviceMode& device() { return controller->deviceMode(); }
    MixMode& mix() { return controller->mixMode(); }
    EncoderBank& bank() { return controller->encoderBank(); }
    juce::UndoManager& undo() { return session.undoManager(); }
    float normalised(int encoder) { return bank().binding(encoder)->normalised(); }
    /** A whole turn: touched, turned, let go. */
    void turn(int encoder, int steps, int moves = 1)
    {
        surface.touch(encoder);
        for (int i = 0; i < moves; ++i)
            surface.turn(encoder, steps);
        surface.untouch(encoder);
        refresh();
    }
    /** The first encoder on the page with a continuous value (no detents), or -1. */
    int continuous()
    {
        for (int e = 0; e < surface.caps.encoders; ++e)
            if (const auto* b = bank().binding(e); b != nullptr && EncoderBank::detents(b->spec()) == 0)
                return e;
        return -1;
    }
    /** The parameter ID an encoder controls (from its address). */
    juce::String parameterOn(int encoder) { return bank().target(encoder).fromLastOccurrenceOf("/", false, false); }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    DeviceSpecs specs{testRegistry()};
    ui::EngineStateFeed feed{session, engine};
    ui::CommandRegistry commands{&session.undoManager()};
    ui::SessionSelectionModel selection;
    FakeSurface surface;
    std::unique_ptr<SurfaceController> controller;
};
} // namespace

TEST_CASE("Device mode: the pages are ParameterPager's for 8, 16 and 32 encoders, and the encoders control them", "[control][device]")
{
    for (const int encoders : {8, 16, 32})
    {
        CAPTURE(encoders);
        Fixture f(presets::virtualPush(8, 8, encoders));
        const auto synth = f.add(0, SynthDevice::typeId);
        REQUIRE(f.device().device() == synth); // the track's first device, with none selected
        const auto expected = ParameterPager::pagesFor(*f.specs.forType(SynthDevice::typeId), encoders);
        const auto pages = f.device().pages();
        REQUIRE(pages.size() == expected.size());
        const auto* spec = f.specs.forType(SynthDevice::typeId);
        for (std::size_t p = 0; p < pages.size(); ++p)
        {
            CAPTURE(p);
            CHECK(pages[p].name == expected[p].name);
            CHECK(pages[p].parameters == expected[p].parameters);
            f.device().setPage(static_cast<int>(p));
            f.refresh();
            for (int e = 0; e < encoders; ++e)
            {
                const bool used = e < static_cast<int>(expected[p].parameters.size());
                const auto id = used ? spec->parameter(expected[p].parameters[static_cast<std::size_t>(e)]).spec().id : juce::String();
                CHECK(f.bank().target(e) == (used ? ui::deviceTarget(synth, id) : juce::String()));
                const auto& cell = f.surface.display.cells[static_cast<std::size_t>(e)];
                CHECK(cell.name == (used ? spec->parameter(expected[p].parameters[static_cast<std::size_t>(e)]).spec().name : juce::String()));
            }
            // The lower buttons: On, then the pages, the current one selected.
            const auto& lower = f.surface.display.lower;
            REQUIRE(lower.size() >= 2);
            CHECK(lower[0].label == "On");
            const int perGroup = static_cast<int>(f.surface.caps.countOf(Role::displayLower)) - 1;
            CHECK(lower[static_cast<std::size_t>(1 + static_cast<int>(p) % perGroup)].selected);
        }
    }
}

TEST_CASE("Device mode: a rack's encoders are its macros in use, any count, paged, with their names", "[control][device]")
{
    Fixture f;
    auto& set = f.session.set();
    auto rack = racks::insertRack(set, f.chain(0), -1, RackDevice::instrumentRackId);
    racks::setMacroCount(set, rack, 12);
    racks::setMacroName(set, rack, 0, "Brightness");
    racks::setMacroColour(set, rack, 0, 0xff30c060);
    f.session.flush();
    f.refresh();
    const auto pages = f.device().pages();
    REQUIRE(pages.size() == 2);
    CHECK(pages[0].name == "Macros 1-8");
    CHECK(pages[1].name == "Macros 9-12");
    CHECK(f.bank().target(0) == ui::deviceTarget(rack, "macro1"));
    CHECK(f.surface.display.cells[0].name == "Brightness");
    CHECK(f.surface.display.cells[0].colour == 0x30c060u);
    CHECK(f.surface.display.cells[1].name == "Macro 2");
    f.surface.click(Role::displayLower, 2); // the second page
    f.refresh();
    CHECK(f.device().page() == 1);
    CHECK(f.bank().target(0) == ui::deviceTarget(rack, "macro9"));
    CHECK(f.bank().target(4).isEmpty());

    f.turn(0, 20);
    CHECK(static_cast<double>(rack.getChildWithName(ids::params)["macro9"]) > 0.0);
}

TEST_CASE("Encoders: a turn is one undo step, Shift turns finely, settings have detents", "[control][device]")
{
    Fixture f;
    f.add(0, SynthDevice::typeId);
    const int e = f.continuous();
    REQUIRE(e >= 0);
    f.bank().binding(e)->setNormalised(0.4f);
    const float before = f.normalised(e);
    const auto resolution = f.surface.caps.encoderResolution;

    // Touched, ten moves of 4 steps, let go: one step back undoes all of it.
    f.turn(e, 4, 10);
    CHECK_THAT(f.normalised(e), WithinAbs(before + 40.0f / static_cast<float>(resolution), 1.0e-3));
    CHECK(f.undo().getUndoDescription().contains(f.bank().binding(e)->spec().name));
    f.undo().undo();
    CHECK_THAT(f.normalised(e), WithinAbs(before, 1.0e-5));

    // Shift held: ten times finer.
    f.surface.down(Role::shift);
    f.turn(e, 40);
    f.surface.up(Role::shift);
    CHECK_THAT(f.normalised(e), WithinAbs(before + 4.0f / static_cast<float>(resolution), 1.0e-3));
    f.undo().undo();

    // Without a touch, a turn ends when the encoder rests: each rest is a step of its own.
    f.surface.turn(e, -10);
    f.surface.turn(e, -10);
    f.bank().endGestures(); // what the rest does
    f.surface.turn(e, -10);
    f.bank().endGestures();
    f.undo().undo();
    CHECK_THAT(f.normalised(e), WithinAbs(before - 20.0f / static_cast<float>(resolution), 1.0e-3));
    f.undo().undo();
    CHECK_THAT(f.normalised(e), WithinAbs(before, 1.0e-5));

    // A choice moves one setting every stepsPerSetting steps, the steps between adding up.
    const auto choice = ParameterSpec::choice("c", "C", {"A", "B", "C", "D"}, 0);
    CHECK(EncoderBank::detents(choice) == 4);
    CHECK_THAT(EncoderBank::stepSize(choice, resolution, false) * EncoderBank::stepsPerSetting * 3, WithinAbs(1.0, 1.0e-9));
    const auto wide = ParameterSpec::integer("i", "I", 0, 127, 0);
    CHECK(EncoderBank::detents(wide) == 0); // too many settings for detents
    CHECK(EncoderBank::stepSize(wide, resolution, false) == 1.0 / resolution);
    const auto pages = f.device().pages();
    const auto* spec = f.specs.forType(SynthDevice::typeId);
    for (std::size_t p = 0; p < pages.size(); ++p)
        for (std::size_t i = 0; i < pages[p].parameters.size(); ++i)
        {
            const auto& s = spec->parameter(pages[p].parameters[i]).spec();
            if (s.kind != ParamKind::Choice || s.choices.size() < 3)
                continue;
            f.device().setPage(static_cast<int>(p));
            f.refresh();
            auto* b = f.bank().binding(static_cast<int>(i));
            REQUIRE(b != nullptr);
            b->setValue(0.0f);
            f.surface.touch(static_cast<int>(i));
            f.surface.turn(static_cast<int>(i), EncoderBank::stepsPerSetting - 1);
            CHECK(b->value() == 0.0f);
            f.surface.turn(static_cast<int>(i), 1);
            CHECK(b->value() == 1.0f);
            f.surface.untouch(static_cast<int>(i));
            return;
        }
    FAIL("the Synth has no choice parameter");
}

TEST_CASE("Encoders: an absolute control picks the value up before it moves it", "[control][device]")
{
    auto caps = presets::apc40Mk2();
    REQUIRE_FALSE(caps.encodersRelative);
    Fixture f(caps);
    f.add(0, SynthDevice::typeId);
    const int e = f.continuous();
    REQUIRE(e >= 0);
    auto* b = f.bank().binding(e);
    b->setNormalised(0.5f);
    f.surface.send(SurfaceInput::encoderSet(e, 0.1f));
    f.surface.send(SurfaceInput::encoderSet(e, 0.2f));
    CHECK_THAT(b->normalised(), WithinAbs(0.5f, 1.0e-5)); // not there yet
    f.surface.send(SurfaceInput::encoderSet(e, 0.6f));    // passed it: picked up
    CHECK_THAT(b->normalised(), WithinAbs(0.6f, 1.0e-3));
    f.surface.send(SurfaceInput::encoderSet(e, 0.7f));
    CHECK_THAT(b->normalised(), WithinAbs(0.7f, 1.0e-3));
}

TEST_CASE("Device mode: the upper buttons choose the device (shared with the Device view), the lower its On switch and pages", "[control][device]")
{
    Fixture f;
    const auto synth = f.add(0, SynthDevice::typeId);
    const auto eq = f.add(0, EqEightDevice::typeId);
    REQUIRE(f.device().device() == synth);
    auto& upper = f.surface.display.upper;
    REQUIRE(upper.size() == 2);
    CHECK(upper[0].label == "Synth");
    CHECK(upper[0].selected);
    CHECK(upper[1].label == "EQ Eight");

    f.surface.click(Role::displayUpper, 1);
    f.refresh();
    CHECK(f.device().device() == eq);
    CHECK(f.selection.selectedDevice() == LiveSet::idOf(eq));
    CHECK(f.surface.button(Role::displayUpper, 1) == LedState::solid(f.controller->colours().white));
    CHECK(f.surface.display.title.contains("EQ Eight"));

    // Chosen elsewhere (the Device view): followed.
    f.selection.selectDevice(LiveSet::idOf(synth));
    f.refresh();
    CHECK(f.device().device() == synth);
    f.selection.selectDevice(LiveSet::idOf(eq));
    f.refresh();

    f.surface.click(Role::displayLower, 0);
    CHECK_FALSE(static_cast<bool>(eq.getProperty(ids::enabled, true)));
    f.refresh();
    CHECK(f.surface.display.lower[0].label == "Off");
    f.undo().undo();
    CHECK(static_cast<bool>(eq.getProperty(ids::enabled, true)));

    REQUIRE(f.device().pages().size() >= 3);
    f.surface.click(Role::displayLower, 3);
    CHECK(f.device().page() == 2);
    // Each device keeps its own page.
    f.surface.click(Role::displayUpper, 0);
    CHECK(f.device().page() == 0);
    f.surface.click(Role::displayUpper, 1);
    CHECK(f.device().page() == 2);

    // Another track: its devices, or none.
    f.selection.set({Kind::track, 1, 0});
    f.refresh();
    CHECK_FALSE(f.device().device().isValid());
    CHECK(f.surface.display.message == "No devices");
    CHECK(f.bank().target(0).isEmpty());
}

TEST_CASE("Device mode: pressing the selected rack shows its chains; a chain goes in, the first button back out", "[control][device]")
{
    Fixture f;
    auto& set = f.session.set();
    auto rack = racks::insertRack(set, f.chain(0), -1, RackDevice::instrumentRackId);
    auto low = racks::addChain(set, rack, "Low");
    auto inner = set.insertDevice(racks::devicesOf(low), -1, SynthDevice::typeId);
    racks::addChain(set, rack, "High"); // empty
    f.session.flush();
    f.refresh();
    REQUIRE(f.device().device() == rack);

    f.surface.click(Role::displayUpper, 0); // the selected rack: its chains
    f.refresh();
    CHECK(f.device().chainsShown() == rack);
    auto upper = f.surface.display.upper;
    REQUIRE(upper.size() == 3);
    CHECK(upper[0].label.endsWith("Instrument Rack"));
    CHECK(upper[1].label == "Low");
    CHECK(upper[2].label == "High");

    f.surface.click(Role::displayUpper, 1); // into Low
    f.refresh();
    CHECK(f.device().device() == inner);
    CHECK(f.selection.selectedDevice() == LiveSet::idOf(inner));
    REQUIRE(f.surface.display.upper.size() == 2);
    CHECK(f.surface.display.upper[1].label == "Synth");
    CHECK(f.bank().target(0).startsWith(ui::deviceTarget(inner, {})));

    f.surface.click(Role::displayUpper, 0); // back out: the rack
    f.refresh();
    CHECK(f.device().device() == rack);

    f.surface.click(Role::displayUpper, 0);
    f.surface.click(Role::displayUpper, 2); // into the empty chain
    f.refresh();
    CHECK_FALSE(f.device().device().isValid());
    CHECK(f.surface.display.message == "Empty chain");
    f.surface.click(Role::displayUpper, 0);
    f.refresh();
    CHECK(f.device().device() == rack);
}

TEST_CASE("Device mode: a touched encoder shows large, and the display is only sent when it changes", "[control][device]")
{
    Fixture f;
    f.add(0, SynthDevice::typeId);
    CHECK(f.surface.display.detail == -1);
    f.surface.touch(2);
    f.refresh();
    CHECK(f.surface.display.detail == 2);
    CHECK(f.surface.display.cells[2].detail.isNotEmpty()); // its range
    const int sent = f.surface.displays;
    f.refresh();
    CHECK(f.surface.displays == sent); // nothing changed
    f.surface.turn(2, 30);
    f.refresh();
    CHECK(f.surface.displays == sent + 1);
    f.surface.untouch(2);
    f.refresh();
    CHECK(f.surface.display.detail == -1);
}

TEST_CASE("Mix mode: volumes, pans, each send and the selected track's page, for the tracks in view", "[control][mix]")
{
    Fixture f;
    auto& set = f.session.set();
    REQUIRE(set.numReturns() >= 2);
    f.surface.click(Role::modeMix);
    f.refresh();
    CHECK(f.controller->encoderModeKind() == ModeKind::mix);
    CHECK(f.controller->modeKind() == ModeKind::session); // the pads stay as they were
    for (int t = 0; t < 4; ++t)
        CHECK(f.bank().target(t) == ui::ValueBinding::targetOf(set.track(t), ids::volumeDb));
    CHECK(f.bank().target(4).isEmpty());
    CHECK(f.surface.display.cells[1].caption == set.track(1)[ids::name].toString());
    CHECK(f.surface.display.cells[1].name == "Volume");
    CHECK(f.surface.display.cells[0].selected); // track 0 is selected
    CHECK(f.surface.display.lower[2].label == set.track(2)[ids::name].toString());

    const double volume = set.track(1)[ids::volumeDb];
    f.turn(1, -40);
    CHECK(static_cast<double>(set.track(1)[ids::volumeDb]) < volume);
    f.undo().undo();
    CHECK(static_cast<double>(set.track(1)[ids::volumeDb]) == volume);

    f.surface.click(Role::displayUpper, 1);
    f.refresh();
    CHECK(f.mix().control() == MixMode::Control::pan);
    CHECK(f.bank().target(0) == ui::ValueBinding::targetOf(set.track(0), ids::pan));
    CHECK(f.surface.display.cells[0].bipolar);

    f.surface.click(Role::displayUpper, 2);
    f.refresh();
    CHECK(f.mix().control() == MixMode::Control::send);
    CHECK(f.bank().target(0) == ui::sendTarget(set.trackId(0), LiveSet::idOf(set.returnTrack(0))));
    f.surface.click(Role::displayUpper, 2); // again: the next send
    f.refresh();
    CHECK(f.mix().send() == 1);
    CHECK(f.bank().target(3) == ui::sendTarget(set.trackId(3), LiveSet::idOf(set.returnTrack(1))));
    CHECK(f.surface.display.upper[2].label == "Send B");
    f.turn(3, 50);
    CHECK(set.sendLevel(3, 1) > LiveSet::minVolumeDb);

    f.surface.click(Role::displayUpper, 3); // the selected track's own page
    f.refresh();
    CHECK(f.mix().control() == MixMode::Control::track);
    CHECK(f.bank().target(0) == ui::ValueBinding::targetOf(set.track(0), ids::volumeDb));
    CHECK(f.bank().target(1) == ui::ValueBinding::targetOf(set.track(0), ids::pan));
    CHECK(f.bank().target(2) == ui::sendTarget(set.trackId(0), LiveSet::idOf(set.returnTrack(0))));
    CHECK(f.surface.display.cells[2].name == set.returnTrack(0)[ids::name].toString());
    f.selection.set({Kind::track, 2, 0});
    f.refresh();
    CHECK(f.bank().target(0) == ui::ValueBinding::targetOf(set.track(2), ids::volumeDb));
}

TEST_CASE("Mix mode: the lower buttons select tracks, or mute and solo them with those held", "[control][mix]")
{
    Fixture f;
    auto& set = f.session.set();
    f.surface.click(Role::modeMix);
    f.surface.click(Role::displayLower, 2);
    CHECK(f.selection.get().track == 2);
    f.surface.down(Role::mute);
    f.surface.click(Role::displayLower, 1);
    f.surface.up(Role::mute);
    CHECK(static_cast<bool>(set.track(1)[ids::mute]));
    f.surface.down(Role::solo);
    f.surface.click(Role::displayLower, 3);
    f.surface.up(Role::solo);
    CHECK(static_cast<bool>(set.track(3)[ids::solo]));
    CHECK(f.selection.get().track == 2);
}

TEST_CASE("Mix mode: the encoders follow the Session box across the set", "[control][mix]")
{
    Fixture f(presets::push2(), 12);
    f.surface.click(Role::modeMix);
    f.refresh();
    CHECK(f.bank().target(0) == ui::ValueBinding::targetOf(f.session.set().track(0), ids::volumeDb));
    f.selection.set({Kind::track, 10, 0}); // the box moves to show it
    f.refresh();
    const auto shown = f.mix().tracks();
    REQUIRE(!shown.empty());
    CHECK(std::find(shown.begin(), shown.end(), 10) != shown.end());
    CHECK(f.bank().target(0) == ui::ValueBinding::targetOf(f.session.set().track(shown.front()), ids::volumeDb));
}

TEST_CASE("Device and Mix modes without encoders: the grid's columns are faders", "[control][mix][device]")
{
    Fixture f(presets::launchpadProMk3());
    auto& set = f.session.set();
    const auto& colours = f.controller->colours();
    f.surface.click(Role::modeMix);
    f.refresh();
    CHECK(f.controller->modeKind() == ModeKind::mix); // it takes the grid
    CHECK_FALSE(f.controller->encoderModeKind().has_value());
    f.surface.tap(0, 1); // the top: all the way up
    CHECK(static_cast<double>(set.track(1)[ids::volumeDb]) == LiveSet::maxVolumeDb);
    f.surface.tap(7, 1);
    CHECK(static_cast<double>(set.track(1)[ids::volumeDb]) == LiveSet::minVolumeDb);
    f.undo().undo();
    CHECK(static_cast<double>(set.track(1)[ids::volumeDb]) == LiveSet::maxVolumeDb);
    f.refresh();
    const auto trackColour = rgbOf(static_cast<std::uint32_t>(static_cast<juce::int64>(set.track(1)[ids::colour])));
    CHECK(f.surface.pad(0, 1) == LedState::solid(trackColour));
    CHECK(f.surface.pad(7, 1) == LedState::solid(trackColour));
    f.surface.tap(7, 1);
    f.refresh();
    CHECK(f.surface.pad(0, 1) == LedState::solid(dimmed(trackColour, 0.12f)));
    CHECK(f.surface.pad(7, 1) == LedState::solid(trackColour));

    f.surface.click(Role::scene, 1); // Pan
    f.refresh();
    CHECK(f.mix().control() == MixMode::Control::pan);
    CHECK(f.surface.button(Role::scene, 1) == LedState::solid(colours.white));
    f.surface.tap(0, 2);
    CHECK(static_cast<double>(set.track(2)[ids::pan]) == 1.0);

    // Device mode: the page's first parameters, the arrows along the chain.
    const auto synth = f.add(0, SynthDevice::typeId);
    const auto eq = f.add(0, EqEightDevice::typeId);
    f.selection.set({Kind::track, 0, 0});
    f.surface.click(Role::modeDevice);
    f.refresh();
    CHECK(f.controller->modeKind() == ModeKind::device);
    CHECK(f.device().device() == synth);
    CHECK(f.device().controls() == 8);
    f.surface.tap(0, 0);
    CHECK_THAT(f.normalised(0), WithinAbs(1.0f, 1.0e-5));
    f.surface.click(Role::navRight);
    f.refresh();
    CHECK(f.device().device() == eq);
    f.surface.click(Role::scene, 1);
    CHECK(f.device().page() == 1);

    // Session again: the pads launch clips as before.
    f.surface.click(Role::modeSession);
    f.refresh();
    CHECK(f.controller->modeKind() == ModeKind::session);
}

TEST_CASE("Surface controller: a pad mode and an encoder mode run together on a device with encoders", "[control][device]")
{
    Fixture f;
    const auto& colours = f.controller->colours();
    CHECK(f.controller->modeKind() == ModeKind::session);
    CHECK(f.controller->encoderModeKind() == ModeKind::device);
    f.surface.click(Role::modeNote);
    f.refresh();
    CHECK(f.controller->modeKind() == ModeKind::note);
    CHECK(f.controller->encoderModeKind() == ModeKind::device);
    f.surface.click(Role::modeMix);
    f.refresh();
    CHECK(f.controller->modeKind() == ModeKind::note);
    CHECK(f.controller->encoderModeKind() == ModeKind::mix);
    CHECK(f.surface.button(Role::modeNote) == LedState::solid(colours.white));
    CHECK(f.surface.button(Role::modeMix) == LedState::solid(colours.white));
    CHECK(f.surface.button(Role::modeDevice) == LedState::solid(dimmed(colours.white, colours.dim)));
    CHECK(f.surface.button(Role::modeSession) == LedState::solid(dimmed(colours.white, colours.dim)));

    // In Session mode Push 2's lower buttons select tracks; under Device mode they're its own.
    f.surface.click(Role::modeSession);
    f.surface.click(Role::modeDevice);
    f.add(1, SynthDevice::typeId);
    f.selection.set({Kind::track, 1, 0});
    f.refresh();
    const auto synth = f.device().device();
    f.surface.click(Role::displayLower, 0);
    CHECK_FALSE(static_cast<bool>(synth.getProperty(ids::enabled, true)));
    CHECK(f.selection.get().track == 1);
}

TEST_CASE("Display: a frame as a text display's lines", "[control][display]")
{
    DisplayFrame frame;
    frame.title = "Bass > Synth";
    frame.cells.push_back({.name = "Cutoff", .value = "1.20 kHz", .position = 0.5f});
    frame.cells.push_back({.name = "Resonance", .value = "40 %", .position = 0.4f});
    auto lines = displayText(frame, 2, 16);
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "Bass > Synth");
    CHECK(lines[1] == "Cutoff 1.20 kHz");
    lines = displayText(frame, 3, 12);
    CHECK(lines[2] == "Resonan 40 %"); // the name shortened, the value whole
    frame.detail = 1;
    lines = displayText(frame, 2, 16);
    CHECK(lines[0] == "Resonance");
    CHECK(lines[1] == "40 %");
}

TEST_CASE("Device and Mix modes on every candidate device: usable, and nothing assumes a size", "[control][device][mix]")
{
    for (const auto& caps : presets::allForTests())
    {
        CAPTURE(caps.name, caps.rows, caps.columns, caps.encoders);
        Fixture f(caps);
        f.add(0, SynthDevice::typeId);
        for (const auto kind : {ModeKind::device, ModeKind::mix})
        {
            f.controller->setMode(kind);
            f.refresh();
            CHECK(f.controller->isShown(kind));
            const int controls = caps.encoders > 0 ? caps.encoders : caps.columns;
            if (controls > 0)
                CHECK(f.bank().binding(0) != nullptr);
            if (caps.encoders > 0)
            {
                f.turn(0, 10);
                CHECK(f.controller->modeKind() == ModeKind::session);
            }
            else if (caps.rows > 0)
            {
                CHECK_FALSE(f.surface.pad(caps.rows - 1, 0).isOff()); // a fader
                f.surface.tap(0, 0);
            }
            if (caps.hasDisplay())
                CHECK(static_cast<int>(f.surface.display.cells.size()) == controls);
        }
        f.controller->setMode(ModeKind::session);
        CHECK(f.controller->modeKind() == ModeKind::session);
    }
}
