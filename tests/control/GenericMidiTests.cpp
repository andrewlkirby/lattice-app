// Generic MIDI controllers (docs/06-m5-controllers.md, 5e): definition files read with their
// mistakes named by place, the encoders' encodings and their detection, the driver turning a
// device's MIDI into pads, buttons and encoders and the modes' lights and words into its MIDI, and
// every included definition running every mode.

#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"
#include "control/generic/GenericMidiDriver.h"
#include "control/surface/PadPlayer.h"
#include "control/surface/SurfaceController.h"
#include "ui/commands/SessionCommands.h"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

using namespace lattice;
using namespace lattice::control;
using namespace lattice::control::generic;
using namespace lattice::test;
using Catch::Matchers::WithinAbs;

namespace
{
struct Recorder final : MidiSender
{
    void send(const juce::MidiMessage& m) override { sent.push_back(m); }
    std::vector<juce::MidiMessage> sent;
    void clear() { sent.clear(); }
    std::vector<std::uint8_t> bytes(std::size_t i) const
    {
        const auto& m = sent[i];
        return {m.getRawData(), m.getRawData() + m.getRawDataSize()};
    }
};

/** The driver's inputs, as the controller would get them. */
struct Inputs
{
    explicit Inputs(GenericMidiDriver& d)
    {
        d.onInput = [this](const SurfaceInput& in) { got.push_back(in); };
    }
    std::vector<SurfaceInput> got;
};

SurfaceDefinition included(const juce::String& name)
{
    for (auto& d : SurfaceDefinition::included())
        if (d.name == name)
            return d;
    FAIL("no included definition " << name);
    return {};
}

SurfaceDefinition parsed(const juce::String& json)
{
    SurfaceDefinition d;
    const auto r = SurfaceDefinition::parse(json, d);
    INFO(r.getErrorMessage());
    REQUIRE(r.wasOk());
    return d;
}

juce::String problemWith(const juce::String& json)
{
    SurfaceDefinition d;
    const auto r = SurfaceDefinition::parse(json, d);
    return r.failed() ? r.getErrorMessage() : juce::String();
}

/** A small definition, with whatever's added (and its own grid if it gives one). */
juce::String small(const juce::String& extra)
{
    const juce::String grid = extra.contains("\"grid\"") ? "" : R"("grid": {"rows": 2, "columns": 2, "first": 10, "rowStep": -2}, )";
    return R"({"lattice": "surface", "version": 1, "name": "Test", "ports": {"input": ["Test"]}, )" + grid + extra + "}";
}

std::vector<std::uint8_t> sysex(std::initializer_list<int> b)
{
    std::vector<std::uint8_t> out;
    for (const int x : b)
        out.push_back(static_cast<std::uint8_t>(x));
    return out;
}
} // namespace

TEST_CASE("Surface definitions: the included ones read, unverified, each control on its own message", "[control][generic]")
{
    const auto all = SurfaceDefinition::included();
    REQUIRE(all.size() == 10); // with Push 2 and 3 (native drivers)
    for (const auto& d : all)
    {
        INFO(d.name);
        CHECK_FALSE(d.verified); // until each is tried on its device
        CHECK(d.source.isNotEmpty());
        CHECK(d.notes.isNotEmpty());
        CHECK(d.isTemplate == d.inputPorts.isEmpty());
        CHECK(static_cast<int>(d.grid.pads.size()) == d.grid.rows * d.grid.columns);
        const auto caps = d.capabilities();
        CHECK(caps.numPads() == static_cast<int>(d.grid.pads.size()));
        CHECK(caps.encoders == static_cast<int>(d.encoders.size()));
        CHECK(caps.buttons.size() == d.buttons.size());
    }
    // The Launchpad X: its top-left pad 81, bottom-right 18; the scene buttons down the right.
    const auto lpx = included("Novation Launchpad X");
    CHECK(lpx.grid.pads.front() == MidiAddress{MidiAddress::Type::note, 0, 81});
    CHECK(lpx.grid.pads.back() == MidiAddress{MidiAddress::Type::note, 0, 18});
    CHECK(lpx.capabilities().countOf(Role::scene) == 8);
    REQUIRE(lpx.init.size() == 1);
    CHECK(lpx.init[0].isSysEx());
    // The APC40 MKII: a track button per channel, all on note 51.
    const auto apc40 = included("Akai APC40 MKII");
    CHECK(apc40.capabilities().countOf(Role::trackSelect) == 8);
    CHECK(apc40.capabilities().rows == 5);
    CHECK(apc40.capabilities().encoders == 16);
}

TEST_CASE("Surface definitions: a mistake is named with where it is", "[control][generic]")
{
    CHECK(problemWith("{").startsWith("not JSON"));
    CHECK(problemWith(R"({"lattice": "surface", "version": 2, "name": "x"})").contains("newer"));
    CHECK(problemWith(R"({"lattice": "surface", "version": 1, "name": "x", "grid": {"rows": 1, "columns": 1, "first": 0, "rowStep": 1}})")
              .contains("ports.input"));
    CHECK(problemWith(small(R"("buttons": [{"role": "play", "cc": 1}, {"role": "jump", "cc": 2}])")).startsWith("buttons[1].role: no role 'jump'"));
    CHECK(problemWith(small(R"("buttons": [{"role": "play", "note": 10}])")).contains("sends the same message as grid pad 0"));
    CHECK(problemWith(small(R"("buttons": [{"role": "play", "cc": 200}])")).startsWith("buttons[0].cc: 200 is outside 0 to 127"));
    CHECK(problemWith(small(R"("init": ["F0 7G F7"])")).startsWith("init[0]: '7G' isn't a hex byte"));
    CHECK(problemWith(small(R"("init": ["F0 01"])")).contains("must end with F7"));
    CHECK(problemWith(small(R"("buttons": [{"role": "scene", "index": 1, "cc": 1}])")).contains("numbered 0, 1, 2"));
    CHECK(problemWith(small(R"("buttons": [{"role": "play", "cc": 1, "led": "rgb"}])")).contains("needs leds.rgb"));
    CHECK(problemWith(small(R"("encoders": [{"cc": 120, "count": 10}])")).contains("runs past 127"));
    CHECK(problemWith(small(R"("encoders": [{"cc": 20, "encoding": "spin"}])")).startsWith("encoders[0].encoding: 'spin'"));
    CHECK(problemWith(small(R"("grid": {"rows": 2, "columns": 2, "first": 120, "rowStep": 10})")).contains("would be 130"));
    CHECK(problemWith(R"({"lattice": "surface", "version": 1, "name": "x", "ports": {"input": ["(unclosed"]}, "encoders": [{"cc": 1}]})")
              .contains("isn't a regular expression"));
    CHECK(problemWith(small(R"("leds": {"rgb": {"kind": "sysex", "template": "F0 {red} F7"}})")).contains("no placeholder {red}"));
    CHECK(problemWith(small(R"("leds": {"rgb": {"kind": "palette", "palette": "rainbow"}})")).contains("no palette 'rainbow'"));
}

TEST_CASE("Encodings: relative steps each way; detection from turning both ways", "[control][generic]")
{
    CHECK(relativeSteps(Encoding::twosComplement, 1) == 1);
    CHECK(relativeSteps(Encoding::twosComplement, 127) == -1);
    CHECK(relativeSteps(Encoding::twosComplement, 120) == -8);
    CHECK(relativeSteps(Encoding::binaryOffset, 65) == 1);
    CHECK(relativeSteps(Encoding::binaryOffset, 63) == -1);
    CHECK(relativeSteps(Encoding::binaryOffset, 64) == 0);
    CHECK(relativeSteps(Encoding::signMagnitude, 3) == 3);
    CHECK(relativeSteps(Encoding::signMagnitude, 65) == -1);
    CHECK(relativeSteps(Encoding::absolute, 40) == 0);
    for (const auto e : {Encoding::absolute, Encoding::twosComplement, Encoding::binaryOffset, Encoding::signMagnitude, Encoding::cc14, Encoding::nrpn14})
        CHECK(encodingFromName(encodingName(e)) == e);

    CHECK(detectEncoding({1, 1, 2, 1, 3}, {127, 127, 126, 125}) == Encoding::twosComplement);
    CHECK(detectEncoding({65, 66, 65, 65}, {63, 63, 62}) == Encoding::binaryOffset);
    CHECK(detectEncoding({1, 2, 1, 1}, {65, 65, 66}) == Encoding::signMagnitude);
    CHECK(detectEncoding({40, 42, 45, 49, 52, 60}, {60, 55, 50, 44}) == Encoding::absolute);
    CHECK_FALSE(detectEncoding({1, 1}, {127, 127}).has_value());   // too few
    CHECK_FALSE(detectEncoding({50, 50, 50}, {50, 50, 50}).has_value()); // says nothing
}

TEST_CASE("Generic driver: pads, buttons, pressure and encoders from the device's MIDI", "[control][generic]")
{
    Recorder out;
    GenericMidiDriver lpx(included("Novation Launchpad X"), out);
    Inputs in(lpx);

    // The top-left pad, then the bottom-left; a note-on at velocity 0 lets go.
    CHECK(lpx.receive(juce::MidiMessage::noteOn(1, 81, static_cast<juce::uint8>(100))));
    CHECK(lpx.receive(juce::MidiMessage::noteOn(1, 11, static_cast<juce::uint8>(127))));
    CHECK(lpx.receive(juce::MidiMessage::aftertouchChange(1, 11, 64)));
    CHECK(lpx.receive(juce::MidiMessage::noteOn(1, 11, static_cast<juce::uint8>(0))));
    // The top row's first button (Up), down and up; a scene button.
    CHECK(lpx.receive(juce::MidiMessage::controllerEvent(1, 91, 127)));
    CHECK(lpx.receive(juce::MidiMessage::controllerEvent(1, 91, 0)));
    CHECK(lpx.receive(juce::MidiMessage::controllerEvent(1, 79, 127)));
    // Not the surface's: passed on.
    CHECK_FALSE(lpx.receive(juce::MidiMessage::controllerEvent(1, 7, 100)));
    CHECK_FALSE(lpx.receive(juce::MidiMessage::noteOn(2, 81, static_cast<juce::uint8>(100))));
    CHECK_FALSE(lpx.receive(juce::MidiMessage::midiClock()));
    CHECK(in.got.empty()); // on the message thread, later
    lpx.deliverPending();
    REQUIRE(in.got.size() == 7);
    using K = SurfaceInput::Kind;
    CHECK((in.got[0].kind == K::padDown && in.got[0].row == 0 && in.got[0].column == 0));
    CHECK_THAT(in.got[0].value, WithinAbs(100.0 / 127.0, 1.0e-5));
    CHECK((in.got[1].kind == K::padDown && in.got[1].row == 7 && in.got[1].column == 0));
    CHECK((in.got[2].kind == K::padPressure && in.got[2].row == 7));
    CHECK_THAT(in.got[2].value, WithinAbs(64.0 / 127.0, 1.0e-5));
    CHECK(in.got[3].kind == K::padUp);
    CHECK((in.got[4].kind == K::buttonDown && in.got[4].button == ButtonId{Role::navUp, 0}));
    CHECK(in.got[5].kind == K::buttonUp);
    CHECK(in.got[6].button == ButtonId{Role::scene, 1});

    // The APC40 MKII: one note, a track button per channel; pads without velocity at full.
    GenericMidiDriver apc(included("Akai APC40 MKII"), out);
    Inputs apcIn(apc);
    CHECK(apc.receive(juce::MidiMessage::noteOn(3, 51, static_cast<juce::uint8>(127))));
    CHECK(apc.receive(juce::MidiMessage::noteOn(1, 32, static_cast<juce::uint8>(20))));
    CHECK(apc.receive(juce::MidiMessage::controllerEvent(1, 17, 127))); // the second device knob, at the top
    apc.deliverPending();
    REQUIRE(apcIn.got.size() == 3);
    CHECK(apcIn.got[0].button == ButtonId{Role::trackSelect, 2});
    CHECK((apcIn.got[1].row == 0 && apcIn.got[1].column == 0 && apcIn.got[1].value == 1.0f));
    CHECK((apcIn.got[2].kind == K::encoderSet && apcIn.got[2].encoder == 1 && apcIn.got[2].value == 1.0f));

    // Relative, 14-bit and NRPN encoders, touch notes, and channel pressure.
    GenericMidiDriver d(parsed(small(R"("grid": {"rows": 1, "columns": 2, "first": 60, "rowStep": 0, "aftertouch": "channel"},
        "encoders": [{"cc": 20, "encoding": "twosComplement", "touch": {"note": 100}},
                     {"cc": 1, "encoding": "cc14"},
                     {"nrpn": 300, "encoding": "nrpn14"}])")),
                        out);
    Inputs dIn(d);
    CHECK(d.capabilities().encoders == 3);
    CHECK_FALSE(d.capabilities().encodersRelative);
    d.receive(juce::MidiMessage::noteOn(1, 100, static_cast<juce::uint8>(127)));
    d.receive(juce::MidiMessage::controllerEvent(1, 20, 125));
    d.receive(juce::MidiMessage::noteOff(1, 100));
    d.receive(juce::MidiMessage::controllerEvent(1, 1, 64)); // the high part waits
    d.receive(juce::MidiMessage::controllerEvent(1, 33, 0));
    for (const auto& [cc, v] : {std::pair{99, 2}, {98, 44}, {6, 127}, {38, 127}}) // parameter 300, all the way
        d.receive(juce::MidiMessage::controllerEvent(1, cc, v));
    d.receive(juce::MidiMessage::noteOn(1, 61, static_cast<juce::uint8>(90)));
    d.receive(juce::MidiMessage::channelPressureChange(1, 100));
    d.deliverPending();
    REQUIRE(dIn.got.size() == 7);
    CHECK((dIn.got[0].kind == K::encoderTouch && dIn.got[0].encoder == 0));
    CHECK((dIn.got[1].kind == K::encoderTurn && dIn.got[1].delta == -3));
    CHECK(dIn.got[2].kind == K::encoderRelease);
    CHECK((dIn.got[3].kind == K::encoderSet && dIn.got[3].encoder == 1));
    CHECK_THAT(dIn.got[3].value, WithinAbs(64.0 * 128.0 / 16383.0, 1.0e-5));
    CHECK((dIn.got[4].kind == K::encoderSet && dIn.got[4].encoder == 2 && dIn.got[4].value == 1.0f));
    CHECK(dIn.got[5].kind == K::padDown);
    CHECK((dIn.got[6].kind == K::padPressure && dIn.got[6].column == 1)); // the held pad only
}

TEST_CASE("Generic driver: lights as palette entries, on/off values or sysex; only what changed; init and exit", "[control][generic]")
{
    Recorder out;
    GenericMidiDriver lpx(included("Novation Launchpad X"), out);
    const auto& caps = lpx.capabilities();

    lpx.connect();
    REQUIRE(!out.sent.empty());
    CHECK(out.sent[0].isSysEx()); // programmer mode first
    CHECK(out.sent.size() == 1 + static_cast<std::size_t>(caps.numPads()) + caps.buttons.size()); // then every light
    out.clear();

    auto frame = LedFrame::blank(caps);
    frame.pad(0, 0, caps.columns) = LedState::solid(0xff0000);
    frame.pad(7, 7, caps.columns) = LedState::blink(0x00ff00);
    frame.pad(1, 0, caps.columns) = LedState::pulse(0x0000ff);
    frame.buttons[static_cast<std::size_t>(caps.indexOf({Role::scene, 0}))] = LedState::solid(0xffffff);
    lpx.showLeds(frame, frame.changesFrom(LedFrame::blank(caps)));
    REQUIRE(out.sent.size() == 4); // nothing else changed
    CHECK(out.bytes(0) == sysex({0x90, 81, 5}));      // red, steady: channel 1
    CHECK(out.bytes(1) == sysex({0x90 | 2, 71, 45})); // blue, pulsing: channel 3
    CHECK(out.bytes(2) == sysex({0x90 | 1, 18, 21})); // green, blinking: channel 2
    CHECK(out.bytes(3) == sysex({0xb0, 89, 3}));      // white on a CC button
    out.clear();

    lpx.disconnect();
    CHECK(out.sent.back().isSysEx()); // back to Live mode last
    CHECK(out.bytes(0) == sysex({0x90, 81, 0}));
    out.clear();

    // The APC mini mk2: pads on channel 7 (full brightness); single-colour buttons on and off.
    GenericMidiDriver apc(included("Akai APC mini mk2"), out);
    const auto& ac = apc.capabilities();
    auto af = LedFrame::blank(ac);
    af.pad(7, 0, ac.columns) = LedState::solid(0xffffff);
    af.buttons[static_cast<std::size_t>(ac.indexOf({Role::trackSelect, 0}))] = LedState::solid(0xff0000);
    af.buttons[static_cast<std::size_t>(ac.indexOf({Role::scene, 0}))] = LedState::blink(0x00ff00);
    apc.showLeds(af, af.changesFrom(LedFrame::blank(ac)));
    REQUIRE(out.sent.size() == 3);
    CHECK(out.bytes(0) == sysex({0x90 | 6, 0, 3}));
    CHECK(out.bytes(1) == sysex({0x90, 100, 1}));
    CHECK(out.bytes(2) == sysex({0x90, 112, 2}));
    out.clear();

    // An RGB sysex light, and a text display a changed line at a time.
    GenericMidiDriver s(parsed(small(R"("grid": {"rows": 1, "columns": 1, "first": 5, "rowStep": 0,
        "led": {"kind": "sysex", "template": "F0 7D 01 {id} {r7} {g7} {b7} {r8hi} {r8lo} F7"}},
        "display": {"lines": 2, "characters": 4, "line": "F0 7D 02 {line} {text} F7"})")),
                        out);
    auto sf = LedFrame::blank(s.capabilities());
    sf.pads[0] = LedState::solid(0xff8000);
    s.showLeds(sf, sf.changesFrom(LedFrame::blank(s.capabilities())));
    REQUIRE(out.sent.size() == 1);
    CHECK(out.bytes(0) == sysex({0xf0, 0x7d, 1, 5, 127, 64, 0, 1, 127, 0xf7}));
    out.clear();
    DisplayFrame df;
    df.title = "Synth";
    df.message = "No devices";
    s.showDisplay(df);
    REQUIRE(out.sent.size() == 2);
    CHECK(out.bytes(0) == sysex({0xf0, 0x7d, 2, 0, 'S', 'y', 'n', 't', 0xf7}));
    out.clear();
    s.showDisplay(df);
    CHECK(out.sent.empty());
}

TEST_CASE("Generic driver: every included definition runs every mode; a pad launches a clip through the fast path", "[control][generic]")
{
    for (const auto& def : SurfaceDefinition::included())
    {
        INFO(def.name);
        if (def.driver.isNotEmpty())
            continue; // a native driver's (PushTests)
        TuningRegistry tunings;
        AudioEngine engine;
        SessionController session{engine, tunings, testRegistry()};
        OfflineRenderer renderer{engine, BlockPattern::Fixed64};
        ui::EngineStateFeed feed{session, engine};
        ui::CommandRegistry commands{&session.undoManager()};
        ui::SessionSelectionModel selection;
        session.newSet(4, 6);
        session.set().createClip(0, 0, 4.0);
        session.flush();
        ui::addSessionCommands(commands, session, {[&] { return selection.get(); }, [&](ui::SessionSelection s) { selection.set(s); }}, {});
        Recorder out;
        GenericMidiDriver driver(def, out);
        driver.connect();
        auto controller = std::make_unique<SurfaceController>(SurfaceContext{session, commands, selection, feed}, driver);
        for (const auto kind : {ModeKind::session, ModeKind::note, ModeKind::device, ModeKind::mix, ModeKind::clip})
        {
            controller->setMode(kind);
            controller->refresh();
        }
        controller->setMode(ModeKind::session);
        controller->refresh();
        const bool lit = def.grid.led.kind != LedOutput::Kind::none ||
                         std::any_of(def.buttons.begin(), def.buttons.end(), [](const auto& b) { return b.ledKind != LedKind::none; });
        CHECK(out.sent.empty() == (!lit && def.init.empty())); // nothing to light, nothing sent

        if (def.grid.pads.empty())
            continue;
        // The top-left pad: the first track's first clip, launched on the device's input thread.
        session.play();
        renderer.renderFor(64);
        const auto& a = def.grid.pads.front();
        const auto press = a.type == MidiAddress::Type::note ? juce::MidiMessage::noteOn(a.channel + 1, a.number, static_cast<juce::uint8>(100))
                                                             : juce::MidiMessage::controllerEvent(a.channel + 1, a.number, 100);
        REQUIRE(driver.receive(press));
        driver.deliverPending();
        session.flush();
        renderer.renderFor(64);
        session.processEngineEvents();
        feed.tick(5.0);
        CHECK(feed.slot(0, 0) != SlotState::Stopped);
        controller.reset();
        driver.disconnect();
    }
}

TEST_CASE("Surface definitions: the published schema lists every role and encoding the reader knows", "[control][generic]")
{
    const auto file = juce::File(LATTICE_SOURCE_DIR).getChildFile("resources/surfaces/surface.schema.json");
    juce::var schema;
    REQUIRE(juce::JSON::parse(file.loadFileAsString(), schema).wasOk());
    const auto list = [](const juce::var& v) {
        juce::StringArray out;
        if (const auto* a = v.getArray())
            for (const auto& x : *a)
                out.add(x.toString());
        return out;
    };
    const auto& props = schema["properties"];
    const auto roles = list(props["buttons"]["items"]["properties"]["role"]["enum"]);
    CHECK(roles.size() == static_cast<int>(Role::count));
    for (int r = 0; r < static_cast<int>(Role::count); ++r)
        CHECK(roles.contains(roleName(static_cast<Role>(r))));
    const auto encodings = list(props["encoders"]["items"]["properties"]["encoding"]["enum"]);
    for (const auto e : {Encoding::absolute, Encoding::twosComplement, Encoding::binaryOffset, Encoding::signMagnitude, Encoding::cc14, Encoding::nrpn14})
        CHECK(encodings.contains(encodingName(e)));
}
