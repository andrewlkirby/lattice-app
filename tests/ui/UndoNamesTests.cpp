// Undo step names (docs/05-m4-ui.md, 4h): each step names what was done and where, in Find's
// location format, so same-named controls on different tracks or devices read differently; a
// single control's step also shows the value it was set to; the name is fixed when the edit is made.

#include "core/model/UndoNames.h"
#include "devices/framework/DeviceSpecs.h"
#include "devices/racks/Rack.h"
#include "devices/racks/RackEditing.h"
#include "engine/SessionController.h"
#include "ui/shell/UndoHistoryView.h"
#include "ui/widgets/ValueBinding.h"

#include <catch2/catch_test_macros.hpp>

using namespace lattice;
using namespace lattice::ui;

namespace
{
struct Fixture
{
    Fixture()
    {
        session.newSet(2, 2);
        session.undoManager().clearUndoHistory();
    }
    LiveSet& set() { return session.set(); }
    juce::UndoManager& undo() { return session.undoManager(); }
    juce::String lastStep() { return undo().getUndoDescriptions()[0]; }

    std::unique_ptr<ValueBinding> parameter(const juce::ValueTree& device, const juce::String& id)
    {
        const auto* spec = specs.forType(device[ids::deviceType].toString());
        REQUIRE(spec != nullptr);
        const auto* p = spec->findParameter(id);
        REQUIRE(p != nullptr);
        return ValueBinding::deviceParameter(set(), device, p->spec());
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings};
    DeviceSpecs specs{DeviceRegistry::builtin()};
};

const juce::String dot = undoNames::dot(), arrow = undoNames::arrow();
const juce::String to = juce::String::fromUTF8(" \xe2\x86\x92 ");
} // namespace

TEST_CASE("Undo names: two same-named knobs on different tracks get distinct steps", "[ui][undo]")
{
    Fixture f;
    const auto a = f.set().insertDevice(LiveSet::chainOf(f.set().track(0)), -1, "lattice.synth");
    const auto b = f.set().insertDevice(LiveSet::chainOf(f.set().track(1)), -1, "lattice.synth");
    auto first = f.parameter(a, "filter.cutoff");
    auto second = f.parameter(b, "filter.cutoff");
    f.undo().clearUndoHistory(); // the inserts

    first->setValue(1200.0f);
    const auto one = f.lastStep();
    second->setValue(1200.0f);
    const auto two = f.lastStep();
    CHECK(one == "Cutoff" + to + first->text() + dot + "Synth" + arrow + "Filter" + dot + "1 MIDI");
    CHECK(two == "Cutoff" + to + second->text() + dot + "Synth" + arrow + "Filter" + dot + "2 MIDI");
    CHECK(one != two);

    // And in the Undo History, each row's action and location.
    UndoHistoryView view(f.undo());
    view.refresh();
    REQUIRE(view.rows().size() == 3);
    CHECK(undoNames::split(view.rows()[1]).action == "Cutoff" + to + first->text());
    CHECK(undoNames::split(view.rows()[1]).location == "Synth" + arrow + "Filter" + dot + "1 MIDI");
    CHECK(undoNames::split(view.rows()[2]).location.endsWith("2 MIDI"));
}

TEST_CASE("Undo names: two same-named devices on one track, told apart by their names", "[ui][undo]")
{
    Fixture f;
    const auto chain = LiveSet::chainOf(f.set().track(0));
    const auto a = f.set().insertDevice(chain, -1, "lattice.compressor");
    const auto b = f.set().insertDevice(chain, -1, "lattice.compressor");
    f.set().setDeviceName(b, "Glue");
    auto first = f.parameter(a, f.specs.forType("lattice.compressor")->parameter(0).spec().id);
    auto second = f.parameter(b, f.specs.forType("lattice.compressor")->parameter(0).spec().id);
    first->resetToDefault();
    first->setNormalised(0.9f);
    const auto one = f.lastStep();
    second->setNormalised(0.9f);
    const auto two = f.lastStep();
    CHECK(undoNames::split(one).location.startsWith("Compressor"));
    CHECK(undoNames::split(two).location.startsWith("Glue"));
}

TEST_CASE("Undo names: a gesture is one step, named for the value it ends at", "[ui][undo]")
{
    Fixture f;
    const auto synth = f.set().insertDevice(LiveSet::chainOf(f.set().track(1)), -1, "lattice.synth");
    auto cutoff = f.parameter(synth, "filter.cutoff");
    cutoff->setValue(500.0f);
    const int before = f.undo().getUndoDescriptions().size();
    const auto earlier = f.lastStep();

    SECTION("moved")
    {
        cutoff->beginGesture();
        cutoff->setValue(800.0f);
        cutoff->setValue(2000.0f);
        cutoff->endGesture();
        CHECK(f.undo().getUndoDescriptions().size() == before + 1);
        CHECK(f.lastStep() == "Cutoff" + to + cutoff->textFor(2000.0f) + dot + "Synth" + arrow + "Filter" + dot + "2 MIDI");
        CHECK(f.undo().getUndoDescriptions()[1] == earlier);
    }
    SECTION("clicked without moving: no step, and the step before keeps its name")
    {
        cutoff->beginGesture();
        cutoff->setValue(500.0f);
        cutoff->endGesture();
        CHECK(f.undo().getUndoDescriptions().size() == before);
        CHECK(f.lastStep() == earlier);
    }
    SECTION("reset and typed")
    {
        cutoff->resetToDefault();
        CHECK(f.lastStep().startsWith("Reset Cutoff" + to + cutoff->textFor(cutoff->spec().defaultValue) + dot));
        cutoff->setFromText("1k");
        CHECK(f.lastStep().startsWith("Cutoff" + to + cutoff->textFor(1000.0f) + dot));
    }
}

TEST_CASE("Undo names: the name is fixed when the edit is made", "[ui][undo]")
{
    Fixture f;
    auto volume = ValueBinding::property(ParameterSpec::continuous("volumeDb", "Volume", -70.0f, 6.0f, 0.0f, ParamUnit::Decibels),
                                         &f.undo(), f.set().track(0), ids::volumeDb,
                                         [&f](double v) { f.set().setTrackVolumeDb(0, v); });
    volume->setValue(-6.0f);
    const auto step = f.lastStep();
    CHECK(step.endsWith(dot + "1 MIDI"));
    f.undo().beginNewTransaction("Rename");
    f.set().setTrackName(0, "Bass");
    CHECK(f.undo().getUndoDescriptions()[1] == step);
    volume->setValue(-3.0f);
    CHECK(f.lastStep().endsWith(dot + "Bass"));
}

TEST_CASE("Undo names: where each kind of node is", "[ui][undo]")
{
    Fixture f;
    auto& set = f.set();
    const auto& registry = DeviceRegistry::builtin();

    // Inside a rack's chain: the device, its chain and the rack, then the track.
    const auto rack = racks::insertRack(set, LiveSet::chainOf(set.track(0)), -1, RackDevice::instrumentRackId);
    racks::addChain(set, rack);
    const auto chain = racks::addChain(set, rack, "Keys");
    const auto inRack = set.insertDevice(racks::devicesOf(chain), -1, "lattice.synth");
    CHECK(undoNames::locationOf(inRack, registry, "Filter") ==
          "Synth" + arrow + "Filter" + dot + "Keys" + dot + "Instrument Rack" + dot + "1 MIDI");
    CHECK(undoNames::locationOf(chain, registry) == "Keys" + dot + "Instrument Rack" + dot + "1 MIDI");
    // The group is the edited device's own, not that of a rack it's in.
    CHECK(undoNames::locationOf(chain, registry, "Mix") == "Keys" + dot + "Instrument Rack" + dot + "1 MIDI");

    // A Drum Rack's pad, by its note.
    const auto drums = racks::insertRack(set, LiveSet::chainOf(set.track(1)), -1, RackDevice::drumRackId);
    const auto pad = racks::addDrumPad(set, drums, 36);
    CHECK(undoNames::locationOf(pad, registry) == "Pad C2" + dot + "Drum Rack" + dot + "2 MIDI");

    // Clips by their scene; scenes; the returns and the master; the set itself has none.
    const auto clip = set.createClip(1, 1, 4.0);
    REQUIRE(clip.isValid());
    CHECK(undoNames::locationOf(clip, registry).endsWith("Scene 2" + dot + "2 MIDI"));
    CHECK(undoNames::locationOf(clip.getChildWithName(ids::notes), registry) == undoNames::locationOf(clip, registry));
    CHECK(undoNames::locationOf(set.scene(0), registry) == "Scene 1");
    CHECK(undoNames::locationOf(set.master(), registry) == "Master");
    CHECK(undoNames::locationOf(set.state(), registry).isEmpty());
    CHECK(undoNames::make("Tempo", set.state(), registry) == "Tempo");

    // Split back into the action and the location.
    const auto parts = undoNames::split("Move clip" + dot + "Clip" + dot + "Scene 2" + dot + "2 MIDI");
    CHECK(parts.action == "Move clip");
    CHECK(parts.location == "Clip" + dot + "Scene 2" + dot + "2 MIDI");
    CHECK(undoNames::split("Tap tempo").location.isEmpty());
}
