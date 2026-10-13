// The generic device editor (docs/04-m3-devices.md, "3f", "Generic editor"), driven headless:
// every device gets a panel with a control for every parameter, edits reach the document, and
// devices, presets and samples go in by the chain rules. "Every parameter can be reached from
// the generic editor with no controller connected."

#include "app/debug/DetailView.h"
#include "app/debug/MixerStrip.h"
#include "devices/racks/Rack.h"
#include "devices/racks/RackEditing.h"
#include "support/FactoryPresets.h"
#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace lattice;
using namespace lattice::debugui;
using namespace lattice::test;

namespace
{
struct Fixture
{
    Fixture()
    {
        settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("lattice-ui", ".settings");
        juce::PropertiesFile::Options options;
        options.applicationName = "LatticeTests";
        settings = std::make_unique<juce::PropertiesFile>(settingsFile, options);
        session.newSet(2, 1);
        for (int t = 0; t < 2; ++t)
        {
            auto c = LiveSet::chainOf(session.set().track(t));
            while (c.getNumChildren() > 0)
                session.set().removeDevice(c.getChild(0));
        }
        session.flush();
        detail = std::make_unique<DetailView>(session, engine, *settings);
        detail->setSize(1600, 420);
        detail->flushUpdates();
    }
    ~Fixture()
    {
        detail = nullptr;
        settings = nullptr;
        settingsFile.deleteFile();
    }

    ChainRow& row() const
    {
        REQUIRE(detail->chainRow() != nullptr);
        return *detail->chainRow();
    }
    void insert(const juce::String& description, int index = -1)
    {
        row().insertFromDescription(description, index);
        detail->flushUpdates();
    }
    int presetIndex(const juce::String& name) const
    {
        const auto& all = detail->presetLibrary().all();
        int found = -1;
        for (std::size_t i = 0; i < all.size(); ++i)
            if (all[i].name == name)
                found = static_cast<int>(i);
        INFO("preset " << name);
        REQUIRE(found >= 0);
        return found;
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer{engine, BlockPattern::Fixed64};
    juce::File settingsFile;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<DetailView> detail;
};

template <typename T>
void collect(juce::Component& c, std::vector<T*>& out)
{
    for (auto* child : c.getChildren())
    {
        if (auto* t = dynamic_cast<T*>(child))
            out.push_back(t);
        collect(*child, out);
    }
}

template <typename T>
std::vector<T*> all(juce::Component& c)
{
    std::vector<T*> out;
    collect(c, out);
    return out;
}

ParameterControl* controlFor(juce::Component& c, const juce::String& id)
{
    for (auto* p : all<ParameterControl>(c))
        if (p->spec().id == id)
            return p;
    return nullptr;
}
} // namespace

TEST_CASE("Device editor: every device type gets a control for every parameter", "[app][editor]")
{
    Fixture f;
    TrackTuning tuning;
    DeviceCreateContext context;
    context.tuning = &tuning;
    for (const auto& type : testRegistry().types())
    {
        if (type.hidden || RackDevice::isRackType(type.id))
            continue;
        INFO(type.id);
        // A fresh track's chain: MIDI effects and instruments need a MIDI chain.
        f.insert(drag::device(type.id));
        auto& row = f.row();
        DevicePanel* panel = nullptr;
        for (auto& p : row.devicePanels())
            if (p->device()[ids::deviceType].toString() == type.id)
                panel = p.get();
        REQUIRE(panel != nullptr);
        const auto instance = testRegistry().create(type.id, context);
        std::set<juce::String> shown;
        for (auto* control : all<ParameterControl>(*panel))
            shown.insert(control->spec().id);
        CHECK(static_cast<int>(shown.size()) == instance->numParameters());
        CHECK(panel->getWidth() == panel->preferredWidth());
        // Clear the chain for the next type.
        f.session.set().removeDevice(panel->device());
        f.detail->flushUpdates();
    }
}

TEST_CASE("Device editor: a knob edits the document, one undo step per drag", "[app][editor]")
{
    Fixture f;
    f.insert(drag::device(SynthDevice::typeId));
    auto* cutoff = controlFor(f.row(), "filter.cutoff");
    REQUIRE(cutoff != nullptr);
    auto sliders = all<juce::Slider>(*cutoff);
    REQUIRE(sliders.size() == 1);
    auto* knob = sliders[0];
    const auto device = f.row().devicePanels()[0]->device();

    REQUIRE(knob->onDragStart);
    knob->onDragStart();
    knob->setValue(0.25, juce::sendNotificationSync);
    knob->setValue(0.75, juce::sendNotificationSync);
    const double atThreeQuarters = LiveSet::deviceParameter(device, "filter.cutoff", 0.0);
    CHECK(atThreeQuarters > 3000.0);
    CHECK(atThreeQuarters < 20000.0);
    f.session.undoManager().undo();
    CHECK(LiveSet::deviceParameter(device, "filter.cutoff", -1.0) == -1.0); // back to no saved value

    // The document drives the knob back (undo, a controller, a preset).
    f.session.set().setDeviceParameter(device, "filter.cutoff", 20.0);
    CHECK(knob->getValue() == 0.0);
}

TEST_CASE("Device editor: drops follow the chain rules; dragging a device's title moves it", "[app][editor]")
{
    Fixture f;
    f.insert(drag::device("lattice.delay"));
    f.insert(drag::device(SynthDevice::typeId), -1);    // goes before the delay
    f.insert(drag::device("lattice.arpeggiator"), 2);   // goes before the synth
    const auto chain = f.row().chain();
    REQUIRE(chain.getNumChildren() == 3);
    CHECK(chain.getChild(0)[ids::deviceType].toString() == "lattice.arpeggiator");
    CHECK(chain.getChild(1)[ids::deviceType].toString() == SynthDevice::typeId);
    CHECK(chain.getChild(2)[ids::deviceType].toString() == "lattice.delay");
    CHECK(f.row().devicePanels().size() == 3);

    // A sample dropped on the chain becomes a Sampler in the instrument's place.
    const auto wav = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("pad", ".wav");
    f.insert(drag::sample(wav), 0);
    CHECK(chain.getChild(1)[ids::deviceType].toString() == "lattice.sampler");
    CHECK(chain.getChild(1).getChildWithName(ids::deviceState)["file"].toString() == wav.getFullPathName());

    // Moving the delay to the front lands it after the instrument; to the master it's allowed.
    const auto delay = chain.getChild(2);
    f.insert(drag::move(LiveSet::idOf(delay)), 0);
    CHECK(chain.indexOf(delay) == 2);
    f.detail->showChainOf({ChainOwner::Kind::Master, 0});
    f.insert(drag::move(LiveSet::idOf(delay)), 0);
    CHECK(LiveSet::chainOwnerOf(delay).hasType(ids::master));
    // A MIDI effect can't go there.
    const auto arp = chain.getChild(0);
    f.insert(drag::move(LiveSet::idOf(arp)), 0);
    CHECK(LiveSet::chainOwnerOf(arp).hasType(ids::track));
}

TEST_CASE("Device editor: a rack preset shows its macros by name and its chain's devices", "[app][editor][racks]")
{
    Fixture f;
    f.insert(drag::preset(f.presetIndex("Colundi Arp Chords")));
    REQUIRE(f.row().devicePanels().size() == 1);
    auto& rackPanel = *f.row().devicePanels()[0];
    const auto rack = rackPanel.device();
    CHECK(racks::macroCount(rack) == 8);

    // Exactly the 8 macros, labelled with their names, then the chain's 4 devices.
    REQUIRE(rackPanel.innerChain() != nullptr);
    CHECK(rackPanel.innerChain()->devicePanels().size() == 4);
    int macros = 0;
    for (auto* c : all<ParameterControl>(rackPanel))
        if (c->spec().id.startsWith("macro"))
            ++macros;
    CHECK(macros == 8);

    // More macros: the panel grows; fewer: mappings beyond go (undo brings them back).
    const int before = rackPanel.preferredWidth();
    f.session.undoManager().beginNewTransaction();
    racks::setMacroCount(f.session.set(), rack, 16);
    f.detail->flushUpdates();
    CHECK(rackPanel.preferredWidth() > before);
    macros = 0;
    for (auto* c : all<ParameterControl>(rackPanel))
        if (c->spec().id.startsWith("macro"))
            ++macros;
    CHECK(macros == 16);

    // A device inside the rack offers its knobs for mapping.
    auto* synthPanel = rackPanel.innerChain()->devicePanels()[2].get();
    auto* resonance = controlFor(*synthPanel, "filter.resonance");
    REQUIRE(resonance != nullptr);
    REQUIRE(resonance->extendMenu);
    juce::PopupMenu menu;
    resonance->extendMenu(menu);
    CHECK(menu.getNumItems() > 0);
}

TEST_CASE("Device editor: Drum Rack pads appear as samples are assigned", "[app][editor][racks]")
{
    Fixture f;
    f.insert(drag::device(RackDevice::drumRackId));
    auto& panel = *f.row().devicePanels()[0];
    CHECK(panel.innerChain() == nullptr); // no pad selected yet
    f.session.undoManager().beginNewTransaction();
    racks::addDrumPad(f.session.set(), panel.device(), 36, {});
    f.detail->flushUpdates();
    // The new pad is selected, showing its Sampler.
    REQUIRE(panel.innerChain() != nullptr);
    REQUIRE(panel.innerChain()->devicePanels().size() == 1);
    CHECK(panel.innerChain()->devicePanels()[0]->device()[ids::deviceType].toString() == "lattice.sampler");
}

TEST_CASE("Mixer: track strips have a send per return; return strips switch pre/post", "[app][mixer]")
{
    Fixture f;
    const int returns = f.session.set().numReturns();
    REQUIRE(returns >= 1);
    MixerStrip track(f.session, f.tunings, MixerStrip::Kind::Track, 0);
    track.setSize(120, MixerStrip::heightFor(returns));
    const auto knobs = all<juce::Slider>(track);
    CHECK(static_cast<int>(knobs.size()) == returns + 2); // sends, pan, volume
    // The send to return A.
    juce::Slider* send = nullptr;
    for (auto* k : knobs)
        if (send == nullptr && k->getTooltip().startsWith("Send to"))
            send = k;
    REQUIRE(send != nullptr);
    send->onDragStart();
    send->setValue(-6.0, juce::sendNotificationSync);
    CHECK(f.session.set().sendLevel(0, 0) == Catch::Approx(-6.0)); // the slider snaps to 0.1 dB steps

    MixerStrip ret(f.session, f.tunings, MixerStrip::Kind::Return, 0);
    ret.setSize(120, MixerStrip::heightFor(returns));
    for (auto* b : all<juce::TextButton>(ret))
        if (b->getButtonText() == "Pre")
            b->onClick();
    CHECK(static_cast<bool>(f.session.set().returnTrack(0)[ids::preFader]));
    CHECK(MixerStrip::heightFor(returns + 3) > MixerStrip::heightFor(returns));
}

TEST_CASE("Device editor: snapshots of the detail view, for looking at", "[.ui-snapshot]")
{
    // lattice_tests "[.ui-snapshot]" writes PNGs to LATTICE_SNAPSHOT_DIR (default: temp).
    Fixture f;
    f.detail->setSize(1800, 430);
    f.insert(drag::device("lattice.arpeggiator"));
    f.insert(drag::device(SynthDevice::typeId));
    f.insert(drag::device("lattice.eq8"));
    f.insert(drag::device("lattice.compressor"));
    f.detail->resized();
    const auto dir = juce::File(juce::SystemStats::getEnvironmentVariable(
        "LATTICE_SNAPSHOT_DIR", juce::File::getSpecialLocation(juce::File::tempDirectory).getFullPathName()));
    const auto save = [&](const juce::String& name) {
        f.detail->flushUpdates();
        f.detail->resized();
        const auto image = f.detail->createComponentSnapshot(f.detail->getLocalBounds());
        juce::PNGImageFormat png;
        const auto file = dir.getChildFile(name + ".png");
        file.deleteFile();
        juce::FileOutputStream out(file);
        REQUIRE(png.writeImageToStream(image, out));
        WARN("Wrote " << file.getFullPathName());
    };
    save("editor-synth-chain");

    f.detail->showChainOf({ChainOwner::Kind::Track, 1});
    f.insert(drag::preset(f.presetIndex("Colundi Arp Chords")));
    save("editor-rack");

    f.session.set().addTrack();
    f.detail->showChainOf({ChainOwner::Kind::Track, 2});
    f.insert(drag::device(RackDevice::drumRackId));
    auto rack = f.row().devicePanels()[0]->device();
    for (const int note : {36, 38, 42, 46})
        racks::addDrumPad(f.session.set(), rack, note, {});
    save("editor-drum-rack");
}
