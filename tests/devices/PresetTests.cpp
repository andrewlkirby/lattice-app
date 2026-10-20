// Presets (docs/04-m3-devices.md, "3f" and "Tests" 1: "A preset saves and loads back
// identically"), the preset library, and the factory Colundi presets.

#include "devices/effects/Compressor.h"
#include "devices/presets/Presets.h"
#include "support/AllocationHooks.h"
#include "support/FactoryPresets.h"
#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <random>

using namespace lattice;
using namespace lattice::test;

namespace
{
struct Fixture
{
    explicit Fixture(int numTracks = 2) : renderer(engine, BlockPattern::Fixed64)
    {
        session.newSet(numTracks, 1);
        for (int t = 0; t < numTracks; ++t)
        {
            auto c = chain(t);
            while (c.getNumChildren() > 0)
                set().removeDevice(c.getChild(0));
            set().setTrackArm(t, true, true);
        }
        session.flush();
        renderer.captureAudio = true;
        violationsBefore = rt::violationCount();
    }
    ~Fixture() { CHECK(rt::violationCount() == violationsBefore); }

    LiveSet& set() { return session.set(); }
    juce::ValueTree chain(int t = 0) { return LiveSet::chainOf(set().track(t)); }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer;
    std::uint64_t violationsBefore = 0;
};

/** A scratch folder, removed afterwards. */
struct TempFolder
{
    TempFolder() : dir(juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("lattice-presets", ""))
    {
        dir.createDirectory();
    }
    ~TempFolder() { dir.deleteRecursively(); }
    juce::File dir;
};

/** The tree without IDs, for comparing a device with its copy. */
juce::ValueTree withoutIds(juce::ValueTree tree)
{
    tree = tree.createCopy();
    std::function<void(juce::ValueTree)> strip = [&](juce::ValueTree node) {
        node.removeProperty(ids::id, nullptr);
        node.removeProperty(ids::targetDevice, nullptr);
        for (auto child : node)
            strip(child);
    };
    strip(tree);
    return tree;
}

/** The same tree, with every value the same (properties loaded from XML are strings). */
bool sameTree(const juce::ValueTree& a, const juce::ValueTree& b)
{
    if (a.getType() != b.getType() || a.getNumProperties() != b.getNumProperties()
        || a.getNumChildren() != b.getNumChildren())
        return false;
    for (int i = 0; i < a.getNumProperties(); ++i)
    {
        const auto name = a.getPropertyName(i);
        if (!b.hasProperty(name))
            return false;
        const auto& va = a[name];
        const auto& vb = b[name];
        const bool numeric = va.isDouble() || va.isInt() || va.isInt64() || vb.isDouble() || vb.isInt() || vb.isInt64();
        // Numbers as the devices get them: parameters are floats (the XML keeps 16 digits, which
        // always restore a float, though not every double).
        if (numeric ? static_cast<float>(static_cast<double>(va)) != static_cast<float>(static_cast<double>(vb))
                    : va.toString() != vb.toString())
        {
            UNSCOPED_INFO(name.toString() << ": " << va.toString() << " vs " << vb.toString());
            return false;
        }
    }
    for (int i = 0; i < a.getNumChildren(); ++i)
        if (!sameTree(a.getChild(i), b.getChild(i)))
            return false;
    return true;
}

presets::Preset roundTrip(const juce::ValueTree& device, const juce::String& name)
{
    presets::Preset p;
    REQUIRE(presets::parse(presets::toXmlString(device, name), p).wasOk());
    return p;
}
} // namespace

TEST_CASE("Presets: every device type saves and loads back identically", "[devices][presets]")
{
    Fixture f(1);
    std::mt19937 rng(7);
    TrackTuning tuning;
    DeviceCreateContext context;
    context.tuning = &tuning;
    for (const auto& type : testRegistry().types())
    {
        INFO(type.id);
        // Every parameter at a random value within its range.
        auto device = f.set().insertDevice(f.chain(), -1, type.id);
        const auto instance = testRegistry().create(type.id, context);
        for (int i = 0; i < instance->numParameters(); ++i)
        {
            const auto& spec = instance->parameter(i).spec();
            const double v = std::uniform_real_distribution<double>(0.0, 1.0)(rng);
            f.set().setDeviceParameter(device, spec.id, spec.constrain(static_cast<float>(spec.minValue + v * (spec.maxValue - spec.minValue))));
        }
        f.set().setDeviceEnabled(device, false);

        const auto file = juce::File::createTempFile(presets::fileExtension);
        REQUIRE(presets::write(device, "Round Trip", file).wasOk());
        presets::Preset p;
        REQUIRE(presets::read(file, p).wasOk());
        file.deleteFile();
        CHECK(p.name == "Round Trip");
        CHECK(sameTree(p.device, device));

        // Inserted on another track: the same values, new IDs.
        auto copy = f.set().insertDeviceCopy(f.chain(), -1, p.device);
        CHECK(LiveSet::idOf(copy) != LiveSet::idOf(device));
        CHECK(sameTree(withoutIds(copy), withoutIds(device)));
        f.set().removeDevice(device);
        f.set().removeDevice(copy);
    }
}

TEST_CASE("Presets: a rack with chains, macros and mappings loads on another track with its mappings intact",
          "[devices][presets][racks]")
{
    Fixture f(2);
    const auto& rackPreset = factoryPresets()[2];
    auto rack = rackPreset.build(f.set(), f.chain(0));
    racks::storeVariation(f.set(), rack, "Start");
    const auto p = roundTrip(rack, "Rack");
    auto copy = f.set().insertDeviceCopy(f.chain(1), -1, p.device);
    f.session.flush();

    CHECK(sameTree(withoutIds(copy), withoutIds(rack)));
    CHECK(racks::macroCount(copy) == 8);
    CHECK(racks::numVariations(copy) == 1);
    // Every mapping points at a device inside the copy, not the original.
    REQUIRE(racks::numMappings(copy) == 8);
    for (const auto& mapping : copy.getChildWithName(ids::mappings))
    {
        const auto target = f.set().deviceById(static_cast<juce::int64>(mapping[ids::targetDevice]));
        REQUIRE(target.isValid());
        bool inside = false;
        for (auto n = target.getParent(); n.isValid(); n = n.getParent())
            inside = inside || n == copy;
        CHECK(inside);
    }
}

TEST_CASE("Presets: sidechain sources stay behind (they name tracks of the set they came from)",
          "[devices][presets]")
{
    Fixture f(2);
    auto comp = f.set().insertDevice(f.chain(1), -1, CompressorDevice::typeId);
    f.set().setDeviceParameter(comp, "comp.sidechain", 1.0);
    auto state = comp.getChildWithName(ids::deviceState);
    state.setProperty(ids::sidechainTrack, static_cast<juce::int64>(f.set().trackId(0)), nullptr);
    state.setProperty(ids::sidechainPreFx, true, nullptr);
    const auto p = roundTrip(comp, "Ducker");
    const auto loaded = p.device.getChildWithName(ids::deviceState);
    CHECK_FALSE(loaded.hasProperty(ids::sidechainTrack));
    CHECK_FALSE(loaded.hasProperty(ids::sidechainPreFx));
    CHECK(LiveSet::deviceParameter(p.device, "comp.sidechain", 0.0) == 1.0); // the switch itself stays
    CHECK(state.hasProperty(ids::sidechainTrack)); // the original is untouched
}

TEST_CASE("Presets: files that aren't presets, or come from a newer version, are refused", "[devices][presets]")
{
    presets::Preset p;
    CHECK(presets::parse("", p).failed());
    CHECK(presets::parse("<LATTICE_SET formatVersion=\"3\"/>", p).failed());
    CHECK(presets::parse("<PRESET formatVersion=\"1\" setFormatVersion=\"3\"/>", p).failed()); // no device
    CHECK(presets::parse("<PRESET formatVersion=\"2\" setFormatVersion=\"3\"><DEVICE type=\"lattice.synth\"/></PRESET>", p)
              .failed());
    CHECK(presets::parse("<PRESET formatVersion=\"1\" setFormatVersion=\"99\"><DEVICE type=\"lattice.synth\"/></PRESET>", p)
              .failed());
    CHECK(presets::parse("<PRESET formatVersion=\"1\" setFormatVersion=\"3\"><DEVICE type=\"lattice.synth\"/></PRESET>", p)
              .wasOk());
    CHECK(presets::read(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("no-such.lpreset"), p)
              .failed());
}

TEST_CASE("Preset library: factory presets, then user presets saved by device", "[devices][presets]")
{
    Fixture f(1);
    TempFolder temp;
    presets::PresetLibrary library(temp.dir, testRegistry());

    // The factory presets are listed first.
    REQUIRE(library.all().size() == factoryPresets().size());
    for (const auto& p : library.all())
        CHECK(p.factory);
    CHECK(library.presetsFor(SynthDevice::typeId).size() == 2);
    CHECK(library.presetsFor(RackDevice::instrumentRackId).size() == 1);

    // A user preset goes in the device's folder; the same name again replaces it.
    auto synth = f.set().insertDevice(f.chain(), -1, SynthDevice::typeId);
    f.set().setDeviceParameter(synth, "filter.cutoff", 500.0);
    juce::File written;
    REQUIRE(library.saveUserPreset(synth, "Dark: Bass?", written).wasOk());
    CHECK(written.getParentDirectory() == temp.dir.getChildFile("Synth"));
    CHECK(written.existsAsFile());
    f.set().setDeviceParameter(synth, "filter.cutoff", 700.0);
    REQUIRE(library.saveUserPreset(synth, "Dark: Bass?", written).wasOk());

    const auto synths = library.presetsFor(SynthDevice::typeId);
    REQUIRE(synths.size() == 3);
    CHECK(synths[0].factory);
    CHECK(synths[1].factory);
    CHECK_FALSE(synths[2].factory);
    CHECK(synths[2].name == "Dark: Bass?");
    presets::Preset loaded;
    REQUIRE(library.load(synths[2], loaded).wasOk());
    CHECK(LiveSet::deviceParameter(loaded.device, "filter.cutoff", 0.0) == 700.0);

    // A newer-version or broken file in the folder is skipped, and the rest still load.
    temp.dir.getChildFile("Synth").getChildFile("Broken.lpreset").replaceWithText("<nope");
    library.rescan();
    CHECK(library.presetsFor(SynthDevice::typeId).size() == 3);
}

TEST_CASE("Factory presets: the embedded files match their builders", "[devices][presets]")
{
    // Fails after a builder (tests/support/FactoryPresets.h) changes until the files are
    // rewritten with lattice_tests "[.write-factory-presets]".
    TempFolder temp;
    presets::PresetLibrary library(temp.dir, testRegistry());
    for (const auto& factory : factoryPresets())
    {
        INFO(factory.name);
        const auto it = std::find_if(library.all().begin(), library.all().end(),
                                     [&](const presets::PresetInfo& p) { return p.name == factory.name; });
        REQUIRE(it != library.all().end());
        presets::Preset embedded, built;
        REQUIRE(library.load(*it, embedded).wasOk());
        REQUIRE(presets::parse(buildFactoryPresetXml(factory), built).wasOk());
        CHECK(sameTree(embedded.device, built.device));
    }
}

TEST_CASE("Factory presets: every one uses only real parameters and plays on a Colundi track",
          "[devices][presets]")
{
    TempFolder temp;
    presets::PresetLibrary library(temp.dir, testRegistry());
    TrackTuning tuning;
    DeviceCreateContext context;
    context.tuning = &tuning;
    for (const auto& info : library.all())
    {
        INFO(info.name);
        presets::Preset p;
        REQUIRE(library.load(info, p).wasOk());

        // Every saved parameter exists on its device, within range.
        std::function<void(const juce::ValueTree&)> check = [&](const juce::ValueTree& node) {
            if (node.hasType(ids::device))
            {
                const auto instance = testRegistry().create(node[ids::deviceType].toString(), context);
                REQUIRE(testRegistry().find(node[ids::deviceType].toString()) != nullptr);
                const auto values = node.getChildWithName(ids::params);
                for (int i = 0; i < values.getNumProperties(); ++i)
                {
                    const auto id = values.getPropertyName(i).toString();
                    INFO(node[ids::deviceType].toString() << " " << id);
                    const auto* parameter = instance->findParameter(id);
                    REQUIRE(parameter != nullptr);
                    const double v = values[values.getPropertyName(i)];
                    CHECK(v >= parameter->spec().minValue);
                    CHECK(v <= parameter->spec().maxValue);
                }
            }
            for (const auto& child : node)
                check(child);
        };
        check(p.device);

        // On a Colundi track, a held chord makes sound (the arp rack's notes come on its beat).
        Fixture f(1);
        f.set().setTrackTuning(0, TuningRegistry::colundiId);
        f.set().insertDeviceCopy(f.chain(), -1, p.device);
        f.session.flush();
        for (const int note : {40, 47, 52})
            f.engine.midiInput().push(juce::MidiMessage::noteOn(1, note, static_cast<juce::uint8>(100)));
        f.renderer.renderFor(96000);
        double peak = 0.0, energy = 0.0;
        bool finite = true;
        for (const float v : f.renderer.audioLeft)
        {
            finite = finite && std::isfinite(v);
            peak = std::max(peak, static_cast<double>(std::abs(v)));
            energy += static_cast<double>(v) * v;
        }
        CHECK(finite);
        CHECK(peak > 0.01);
        CHECK(peak < 1.0);
        CHECK(energy > 1.0);
    }
}

TEST_CASE("Write the factory presets into resources/presets", "[.write-factory-presets]")
{
    const auto folder = juce::File(LATTICE_SOURCE_DIR).getChildFile("resources").getChildFile("presets");
    REQUIRE(folder.createDirectory().wasOk());
    for (const auto& factory : factoryPresets())
    {
        const auto file = folder.getChildFile(factory.file);
        REQUIRE(file.replaceWithText(buildFactoryPresetXml(factory), false, false, "\n"));
        WARN("Wrote " << file.getFullPathName());
    }
}
