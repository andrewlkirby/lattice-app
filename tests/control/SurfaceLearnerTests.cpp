// The learn wizard's work (docs/06-m5-controllers.md, 5e, "Learn wizard") and the definition
// writer: every included definition written and read back the same; a grid worked out from three
// presses or learned pad by pad; encoders' encodings from their values; buttons given roles; the
// lights tried each way; the result a file that reads and drives the device.

#include "control/generic/GenericMidiDriver.h"
#include "ui/controllers/SurfaceLearner.h"

#include <catch2/catch_test_macros.hpp>

using namespace lattice;
using namespace lattice::control;
using namespace lattice::control::generic;
using ui::SurfaceLearner;

namespace
{
juce::MidiMessage on(int note, int velocity = 100, int channel = 1)
{
    return juce::MidiMessage::noteOn(channel, note, static_cast<juce::uint8>(velocity));
}
juce::MidiMessage cc(int number, int value, int channel = 1)
{
    return juce::MidiMessage::controllerEvent(channel, number, value);
}
} // namespace

TEST_CASE("Definition writer: every included definition is written and read back the same", "[control][generic][learn]")
{
    for (const auto& d : SurfaceDefinition::included())
    {
        INFO(d.name);
        const auto json = d.toJson();
        SurfaceDefinition back;
        const auto r = SurfaceDefinition::parse(json, back);
        INFO(r.getErrorMessage());
        REQUIRE(r.wasOk());
        CHECK(back.toJson() == json);
        CHECK(back.grid == d.grid);
        CHECK(back.buttons == d.buttons);
        CHECK(back.encoders == d.encoders);
        CHECK(back.singleLed == d.singleLed);
        CHECK(back.rgbLed == d.rgbLed);
        CHECK(back.inputPorts == d.inputPorts);
        CHECK(back.init.size() == d.init.size());
    }
}

TEST_CASE("Learner: an evenly numbered grid from three presses; encoders by their values; buttons and lights", "[control][generic][learn]")
{
    SurfaceLearner l;
    l.start("My Grid", "Grid Port", "Grid Port", 8, 8);
    REQUIRE(l.step() == SurfaceLearner::Step::grid);
    CHECK(l.instruction().contains("top-left"));
    // A Launchpad's numbering: top-left 81, the next 82, bottom-right 18; velocities vary.
    l.heard(on(81, 90));
    l.heard(on(81, 0)); // its release: nothing
    CHECK(l.instruction().contains("right"));
    l.heard(on(82, 60));
    l.heard(juce::MidiMessage::aftertouchChange(1, 82, 40));
    l.heard(on(18, 120));
    REQUIRE(l.step() == SurfaceLearner::Step::encoders);
    auto d = l.definition();
    REQUIRE(d.grid.pads.size() == 64);
    CHECK(d.grid.pads[8].number == 71);
    CHECK(d.grid.pads[63].number == 18);
    CHECK(d.grid.velocity);
    CHECK(d.grid.aftertouch == Aftertouch::polyphonic);

    // A relative encoder (two's complement), then a 14-bit fader, then an NRPN knob.
    for (const int v : {1, 1, 2, 1})
        l.heard(cc(21, v));
    l.encoderTurnedClockwise();
    for (const int v : {127, 127, 126})
        l.heard(cc(21, v));
    REQUIRE(l.encoderDone());
    for (const int v : {10, 20, 30})
    {
        l.heard(cc(1, v));
        l.heard(cc(33, 5));
    }
    l.encoderTurnedClockwise();
    l.heard(cc(1, 20));
    l.heard(cc(33, 5));
    REQUIRE(l.encoderDone());
    for (const auto& [n, v] : {std::pair{99, 1}, {98, 10}, {6, 40}, {38, 0}, {6, 41}, {38, 0}})
        l.heard(cc(n, v));
    l.encoderTurnedClockwise();
    l.heard(cc(6, 39));
    REQUIRE(l.encoderDone());
    // Values that don't say: it asks again.
    l.heard(cc(22, 64));
    l.heard(cc(22, 64));
    l.encoderTurnedClockwise();
    l.heard(cc(22, 64));
    CHECK_FALSE(l.encoderDone());
    CHECK(l.lastHeard().contains("turn it a few more times"));
    l.encodersFinished();
    d = l.definition();
    REQUIRE(d.encoders.size() == 3);
    CHECK(d.encoders[0].encoding == Encoding::twosComplement);
    CHECK(d.encoders[1].encoding == Encoding::cc14);
    CHECK(d.encoders[1].lsb == 33);
    CHECK(d.encoders[2].encoding == Encoding::nrpn14);
    CHECK(d.encoders[2].nrpn == 138);

    // Buttons: a press, then its role; a pad isn't a button; a numbered role in order.
    REQUIRE(l.step() == SurfaceLearner::Step::buttons);
    l.heard(on(81));
    CHECK_FALSE(l.hasPendingButton());
    CHECK(l.lastHeard().contains("already learned"));
    l.heard(cc(91, 127));
    REQUIRE(l.hasPendingButton());
    CHECK_FALSE(l.assign({Role::scene, 1})); // scene 0 first
    CHECK(l.assign({Role::navUp, 0}));
    l.heard(cc(89, 127));
    CHECK(l.assign({Role::scene, 0}));
    l.heard(cc(79, 127));
    CHECK_FALSE(l.assign({Role::scene, 0})); // taken
    CHECK(l.assign({Role::scene, 1}));
    l.buttonsFinished();

    // Lights: each way tried lights the corners; the palette one kept.
    REQUIRE(l.step() == SurfaceLearner::Step::lights);
    const auto pattern = l.testPattern(0);
    REQUIRE(pattern.size() == 64 + 3);
    CHECK(pattern[0].getNoteNumber() == 81);
    CHECK(pattern[0].getVelocity() == 5); // red
    CHECK(l.testPattern(4).empty());       // no lights
    l.chooseLights(0);
    CHECK(l.step() == SurfaceLearner::Step::done);

    // The file: it reads, and drives the device as learned.
    d = l.definition();
    CHECK_FALSE(d.verified);
    SurfaceDefinition back;
    const auto r = SurfaceDefinition::parse(d.toJson(), back);
    INFO(r.getErrorMessage());
    REQUIRE(r.wasOk());
    CHECK(back.inputPorts[0] == "Grid Port");
    CHECK(back.capabilities().countOf(Role::scene) == 2);
    struct Quiet final : MidiSender
    {
        void send(const juce::MidiMessage&) override {}
    } quiet;
    GenericMidiDriver driver(back, quiet);
    std::vector<SurfaceInput> got;
    driver.onInput = [&](const SurfaceInput& in) { got.push_back(in); };
    driver.receive(on(71));
    driver.receive(cc(79, 127));
    driver.receive(cc(21, 127));
    driver.deliverPending();
    REQUIRE(got.size() == 3);
    CHECK((got[0].row == 1 && got[0].column == 0));
    CHECK(got[1].button == ButtonId{Role::scene, 1});
    CHECK((got[2].kind == SurfaceInput::Kind::encoderTurn && got[2].delta == -1));
}

TEST_CASE("Learner: an unevenly numbered grid is learned pad by pad; no velocity when every press is full", "[control][generic][learn]")
{
    SurfaceLearner l;
    l.start("Odd", "Odd Port", "", 3, 3);
    l.heard(on(10, 127));
    l.heard(on(11, 127));
    l.heard(on(41, 127)); // 10, 11 ... 41: the step down would be 14.5, so not even
    REQUIRE(l.learningPadByPad());
    CHECK(l.padsLearned() == 1); // the top-left kept
    CHECK(l.instruction().contains("row 1, column 2"));
    l.heard(on(11, 127));
    l.heard(on(11, 127)); // twice: ignored
    CHECK(l.lastHeard().contains("already pressed"));
    for (const int n : {12, 20, 21, 22, 30, 31, 41})
        l.heard(on(n, 127));
    REQUIRE(l.step() == SurfaceLearner::Step::encoders);
    const auto d = l.definition();
    REQUIRE(d.grid.pads.size() == 9);
    CHECK(d.grid.pads[3].number == 20);
    CHECK(d.grid.pads[8].number == 41);
    CHECK_FALSE(d.grid.velocity);
    CHECK(d.outputPorts == d.inputPorts);
    // Written as a table, and read back the same.
    const auto json = d.toJson();
    CHECK(json.contains("table"));
    SurfaceDefinition back;
    REQUIRE(SurfaceDefinition::parse(json, back).wasOk());
    CHECK(back.grid.pads == d.grid.pads);

    // No grid: straight to the encoders.
    l.start("Knobs", "K", "K", 0, 0);
    CHECK(l.step() == SurfaceLearner::Step::encoders);
}
