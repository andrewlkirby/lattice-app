// MIDI Map and Key Map (docs/05-m4-ui.md, 4g): learning a mapping, saving it in the set, a CC moving
// a device's parameter, Pickup that doesn't jump, Value Scaling, relative encoders, ranges, notes and
// keys toggling, slots and commands pressed, a knob's turn as one undo step, and the shell's mode.

#include "devices/framework/DeviceSpecs.h"
#include "support/TestDevices.h"
#include "ui/mapping/Mappings.h"
#include "ui/shell/MainShell.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace lattice;
using namespace lattice::ui;
using namespace lattice::test;
using Catch::Approx;
using Mode = Mappings::Mode;

namespace
{
juce::MidiMessage cc(int number, int value, int channel = 1)
{
    return juce::MidiMessage::controllerEvent(channel, number, value);
}

struct Fixture
{
    Fixture()
    {
        session.newSet(2, 2);
        synth = session.set().insertDevice(LiveSet::chainOf(session.set().track(0)), -1, "lattice.synth");
        // A continuous parameter of the Synth's (not a choice or a switch).
        const auto* device = specs.forType("lattice.synth");
        for (int i = 0; i < device->numParameters(); ++i)
            if (device->parameter(i).spec().kind == ParamKind::Float && device->parameter(i).spec().interval == 0.0f)
            {
                parameter = device->parameter(i).spec().id;
                break;
            }
        REQUIRE(parameter.isNotEmpty());
        target = "device:" + juce::String(LiveSet::idOf(synth)) + "/" + parameter;
    }

    /** Maps CC `number` to the parameter (as clicking it in MIDI Map mode, then moving the knob). */
    juce::ValueTree learnCc(int number)
    {
        mappings.setMode(Mode::midi);
        mappings.setLearnTarget(target, "Synth > " + parameter);
        REQUIRE(mappings.handleMidi("port", cc(number, 10)));
        mappings.setMode(Mode::off);
        auto all = mappings.forTarget(target);
        REQUIRE(all.size() == 1);
        return all.front();
    }

    double value()
    {
        const auto& spec = specs.forType("lattice.synth")->findParameter(parameter)->spec();
        const auto range = spec.normalisableRange();
        return range.convertTo0to1(static_cast<float>(LiveSet::deviceParameter(synth, parameter, spec.defaultValue)));
    }
    void setValue(double normalised)
    {
        const auto& spec = specs.forType("lattice.synth")->findParameter(parameter)->spec();
        session.set().setDeviceParameter(synth, parameter, spec.normalisableRange().convertFrom0to1(static_cast<float>(normalised)));
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings};
    CommandRegistry commands{&session.undoManager()};
    DeviceSpecs specs{DeviceRegistry::builtin()};
    Mappings mappings{session, commands, specs};
    juce::ValueTree synth;
    juce::String parameter, target;
};
} // namespace

TEST_CASE("MIDI Map: a learnt CC moves the parameter, and the mapping is saved in the set", "[ui][mapping]")
{
    Fixture f;
    const auto m = f.learnCc(21);
    CHECK(Mappings::sourceText(m) == juce::String::fromUTF8("CC 21 \xc2\xb7 Ch 1"));

    f.setValue(0.0);
    f.mappings.handleMidi("port", cc(21, 0)); // picked up at the value
    f.mappings.handleMidi("port", cc(21, 64));
    CHECK(f.value() == Approx(64.0 / 127.0).margin(0.01));
    f.mappings.handleMidi("port", cc(21, 127));
    CHECK(f.value() == Approx(1.0).margin(0.01));
    CHECK(!f.mappings.handleMidi("port", cc(22, 5))); // not mapped
    CHECK(!f.mappings.handleMidi("port", cc(21, 5, 2))); // another channel

    // Saved and loaded with the set.
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("mapped", ".lset");
    REQUIRE(f.session.saveSet(file).wasOk());
    REQUIRE(f.session.loadSet(file).wasOk());
    REQUIRE(f.mappings.all().size() == 1);
    CHECK(f.mappings.all()[0][juce::Identifier("target")].toString().startsWith("device:"));
    file.deleteFile();
}

TEST_CASE("MIDI Map: Pickup waits for the control to reach the value; Value Scaling follows it", "[ui][mapping]")
{
    Fixture f;
    const auto m = f.learnCc(1);
    f.setValue(0.5);
    SECTION("Pickup")
    {
        for (const int v : {0, 20, 40, 60})
        {
            f.mappings.handleMidi("port", cc(1, v));
            CHECK(f.value() == Approx(0.5)); // below the value: nothing yet
        }
        // Passing 0.5 (between 60 and 70) picks it up.
        f.mappings.handleMidi("port", cc(1, 70));
        CHECK(f.value() == Approx(70.0 / 127.0).margin(0.01));
        f.mappings.handleMidi("port", cc(1, 30));
        CHECK(f.value() == Approx(30.0 / 127.0).margin(0.01));
    }
    SECTION("Value Scaling")
    {
        f.mappings.setTakeover(m, Mappings::Takeover::scaling);
        f.mappings.handleMidi("port", cc(1, 0)); // where the control is
        CHECK(f.value() == Approx(0.5));
        f.mappings.handleMidi("port", cc(1, 64)); // half way up: half of the way left
        CHECK(f.value() == Approx(0.5 + 0.5 * 64.0 / 127.0).margin(0.01));
        f.mappings.handleMidi("port", cc(1, 127)); // they meet at the top
        CHECK(f.value() == Approx(1.0).margin(0.01));
        f.mappings.handleMidi("port", cc(1, 0)); // and at the bottom
        CHECK(f.value() == Approx(0.0).margin(0.01));
    }
}

TEST_CASE("MIDI Map: relative encoders step the value; ranges limit it", "[ui][mapping]")
{
    Fixture f;
    const auto m = f.learnCc(5);
    f.setValue(0.5);
    const double step = 1.0 / 127.0;
    f.mappings.setEncoder(m, Mappings::Encoder::twosComplement);
    f.mappings.handleMidi("port", cc(5, 1));
    CHECK(f.value() == Approx(0.5 + step).margin(1e-4));
    f.mappings.handleMidi("port", cc(5, 127));
    CHECK(f.value() == Approx(0.5).margin(1e-4));
    f.mappings.setEncoder(m, Mappings::Encoder::binaryOffset);
    f.mappings.handleMidi("port", cc(5, 66));
    CHECK(f.value() == Approx(0.5 + 2 * step).margin(1e-4));
    f.mappings.setEncoder(m, Mappings::Encoder::signMagnitude);
    f.mappings.handleMidi("port", cc(5, 65));
    CHECK(f.value() == Approx(0.5 + step).margin(1e-4));

    // A range: the control's travel covers 20-40% of the value.
    f.mappings.setEncoder(m, Mappings::Encoder::absolute);
    f.mappings.setTakeover(m, Mappings::Takeover::scaling);
    f.mappings.setRange(m, 0.2, 0.4);
    f.setValue(0.2);
    f.mappings.handleMidi("port", cc(5, 0));
    f.mappings.handleMidi("port", cc(5, 127));
    CHECK(f.value() == Approx(0.4).margin(0.01));
    f.mappings.handleMidi("port", cc(5, 0));
    CHECK(f.value() == Approx(0.2).margin(0.01));
}

TEST_CASE("MIDI Map: a knob's turn is one undo step", "[ui][mapping]")
{
    Fixture f;
    f.learnCc(7);
    f.setValue(0.0);
    f.session.undoManager().beginNewTransaction("Before");
    for (int v = 0; v <= 127; v += 8)
        f.mappings.handleMidi("port", cc(7, v));
    f.mappings.endGestures();
    CHECK(f.value() > 0.9);
    f.session.undoManager().undo();
    CHECK(f.value() == Approx(0.0).margin(0.01));
}

TEST_CASE("MIDI Map: notes toggle values and press slots, scenes and commands; a source has one target", "[ui][mapping]")
{
    Fixture f;
    auto& set = f.session.set();
    // A note on the track's on switch (mute, shown as "on").
    const auto mute = "node:" + juce::String(set.trackId(1)) + "/mute";
    f.mappings.setMode(Mode::midi);
    f.mappings.setLearnTarget(mute, "2 MIDI > Track On");
    f.mappings.handleMidi("port", juce::MidiMessage::noteOn(10, 36, static_cast<juce::uint8>(100)));
    f.mappings.setMode(Mode::off);
    CHECK(!static_cast<bool>(set.track(1)[ids::mute]));
    f.mappings.handleMidi("port", juce::MidiMessage::noteOn(10, 36, static_cast<juce::uint8>(100)));
    CHECK(static_cast<bool>(set.track(1)[ids::mute]));
    f.mappings.handleMidi("port", juce::MidiMessage::noteOff(10, 36));
    CHECK(static_cast<bool>(set.track(1)[ids::mute])); // a note's end doesn't toggle back
    f.mappings.handleMidi("port", juce::MidiMessage::noteOn(10, 36, static_cast<juce::uint8>(100)));
    CHECK(!static_cast<bool>(set.track(1)[ids::mute]));

    // A command.
    f.commands.add({.id = "transport.metronome",
                    .name = "Metronome",
                    .category = "Transport",
                    .perform = [&set] { set.setMetronomeOn(!set.metronomeOn()); }});
    f.mappings.setMode(Mode::midi);
    f.mappings.setLearnTarget("command:transport.metronome", "Metronome");
    f.mappings.handleMidi("port", juce::MidiMessage::noteOn(10, 37, static_cast<juce::uint8>(100)));
    f.mappings.setMode(Mode::off);
    const bool before = set.metronomeOn();
    f.mappings.handleMidi("port", juce::MidiMessage::noteOn(10, 37, static_cast<juce::uint8>(100)));
    CHECK(set.metronomeOn() != before);

    // The same note learnt for another target moves there.
    f.mappings.setMode(Mode::midi);
    f.mappings.setLearnTarget("slot:" + juce::String(set.trackId(0)) + "/1", "1 MIDI > Slot 2");
    f.mappings.handleMidi("port", juce::MidiMessage::noteOn(10, 37, static_cast<juce::uint8>(100)));
    f.mappings.setMode(Mode::off);
    CHECK(f.mappings.forTarget("command:transport.metronome").empty());
    CHECK(f.mappings.all().size() == 2);
    // Pressing it launches the slot (an empty slot on a track that isn't armed: its stop button).
    CHECK(f.mappings.handleMidi("port", juce::MidiMessage::noteOn(10, 37, static_cast<juce::uint8>(100))));

    // Removing it, undone.
    const auto slotMapping = f.mappings.forTarget("slot:" + juce::String(set.trackId(0)) + "/1").front();
    f.mappings.remove(slotMapping);
    CHECK(f.mappings.all().size() == 1);
    f.session.undoManager().undo();
    CHECK(f.mappings.all().size() == 2);
}

TEST_CASE("Key Map: a key toggles a value; the computer keyboard keeps its keys", "[ui][mapping]")
{
    Fixture f;
    auto& set = f.session.set();
    const auto solo = "node:" + juce::String(set.trackId(0)) + "/solo";
    const auto key = juce::KeyPress('1', juce::ModifierKeys::noModifiers, '1');
    f.mappings.isKeyboardKey = [](const juce::KeyPress& k) { return k.getTextCharacter() == 'a'; };
    f.mappings.setMode(Mode::key);
    f.mappings.setLearnTarget(solo, "1 MIDI > Solo");
    CHECK(f.mappings.handleKey(key));
    CHECK(!f.mappings.handleKey(juce::KeyPress('a', 0, 'a'))); // the keyboard's: not learnt
    f.mappings.setMode(Mode::off);
    REQUIRE(f.mappings.forTarget(solo).size() == 1);
    CHECK(Mappings::sourceText(f.mappings.forTarget(solo)[0]).startsWith("Key "));

    // Saved and loaded with the set (track IDs are kept in a set's file).
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("keymapped", ".lset");
    REQUIRE(f.session.saveSet(file).wasOk());
    REQUIRE(f.session.loadSet(file).wasOk());
    file.deleteFile();
    REQUIRE(f.mappings.forTarget(solo).size() == 1);
    CHECK(f.mappings.handleKey(key));
    CHECK(static_cast<bool>(set.track(0)[ids::solo]));
    CHECK(f.mappings.handleKey(key));
    CHECK(!static_cast<bool>(set.track(0)[ids::solo]));
    CHECK(!f.mappings.handleKey(juce::KeyPress('2', 0, '2')));
}

TEST_CASE("MIDI Map in the shell: Ctrl+M, the overlay's targets, the list's menu", "[ui][mapping]")
{
    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    session.newSet(2, 2);
    const auto settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("lattice-map", ".settings");
    juce::PropertiesFile::Options options;
    options.applicationName = "LatticeTests";
    juce::PropertiesFile settings(settingsFile, options);
    {
        MainShell shell(session, engine, settings, tunings, ShellActions{});
        shell.setSize(1280, 900);
        shell.sessionView().flushUpdates();
        CHECK(shell.commands().keyMappings().keyPressed(juce::KeyPress('m', juce::ModifierKeys::commandModifier, 0), &shell));
        CHECK(shell.mappings().mode() == Mode::midi);
        CHECK(shell.mapOverlay().isVisible());
        CHECK(shell.mappingsView().isVisible());

        // The overlay names the fader under a point (without a window, it's found by bounds).
        auto* strip = shell.sessionView().stripForColumn(0);
        REQUIRE(strip != nullptr);
        auto* fader = strip->fader();
        REQUIRE(fader != nullptr);
        CHECK(fader->binding().target == "node:" + juce::String(session.set().trackId(0)) + "/volumeDb");

        // A mapping's menu: range, takeover, encoder, remove.
        shell.mappings().setLearnTarget(fader->binding().target, "1 MIDI > Volume");
        shell.mappings().handleMidi("port", cc(10, 0));
        REQUIRE(shell.mappings().all().size() == 1);
        juce::StringArray items;
        const auto menu = shell.mappingsView().menuFor(shell.mappings().all()[0]);
        for (juce::PopupMenu::MenuItemIterator it(menu); it.next();)
            items.add(it.getItem().text);
        for (const auto* expected : {"Range", "Takeover", "Encoder", "Remove"})
            CHECK(items.contains(expected));

        CHECK(shell.commands().perform("view.midiMap"));
        CHECK(shell.mappings().mode() == Mode::off);
        CHECK(!shell.mapOverlay().isVisible());
    }
    settingsFile.deleteFile();
}
