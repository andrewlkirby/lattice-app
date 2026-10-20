// Device framework tests (docs/04-m3-devices.md, "Tests" 1 and 2): parameters and pages, device
// state in the document, the on/off switch, and chain changes while playing.

#include "core/SettledShared.h"
#include "devices/instruments/Sampler.h"
#include "devices/instruments/Synth.h"
#include "devices/midi/Arpeggiator.h"
#include "devices/test/TestSynthDevice.h"
#include "devices/framework/ParameterPager.h"
#include "support/AllocationHooks.h"
#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <set>

using namespace lattice;
using namespace lattice::test;

namespace
{
/** A session (with the test devices available) and an OfflineRenderer; checks on destruction that
    nothing allocated on the audio thread. Tracks start with an empty chain. */
struct Fixture
{
    explicit Fixture(int numTracks = 1, BlockPattern pattern = BlockPattern::Fixed64)
        : renderer(engine, pattern)
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
    juce::ValueTree add(const juce::String& type, int track = 0, int index = -1)
    {
        auto d = set().insertDevice(chain(track), index, type);
        session.flush();
        return d;
    }
    template <typename T>
    T* instance(const juce::ValueTree& device)
    {
        return dynamic_cast<T*>(session.snapshots().deviceFor(LiveSet::idOf(device)));
    }
    std::shared_ptr<TrackProcessor> processor(int track = 0) { return session.processorForTrack(track); }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer;
    std::uint64_t violationsBefore = 0;
};

float largestStep(const std::vector<float>& signal, std::size_t from = 1)
{
    float largest = 0.0f;
    for (auto i = std::max<std::size_t>(from, 1); i < signal.size(); ++i)
        largest = std::max(largest, std::abs(signal[i] - signal[i - 1]));
    return largest;
}

float peak(const std::vector<float>& signal)
{
    float p = 0.0f;
    for (const float v : signal)
        p = std::max(p, std::abs(v));
    return p;
}

std::vector<ParameterGroup> groupsOfSizes(std::initializer_list<int> sizes)
{
    std::vector<ParameterGroup> groups;
    int next = 0;
    int number = 0;
    for (const int size : sizes)
    {
        ParameterGroup g;
        g.name = "G" + juce::String(++number);
        for (int i = 0; i < size; ++i)
            g.parameters.push_back(next++);
        groups.push_back(g);
    }
    return groups;
}
} // namespace

// --- Parameters ----------------------------------------------------------------------------------------

TEST_CASE("ParameterPager: every parameter on exactly one page, in order, within the encoder count",
          "[devices][framework]")
{
    const auto groups = groupsOfSizes({3, 12, 1, 5, 40, 2, 8, 0, 4, 4});
    std::vector<int> expectedOrder;
    for (const auto& g : groups)
        expectedOrder.insert(expectedOrder.end(), g.parameters.begin(), g.parameters.end());

    for (const int encoders : {4, 8, 16, 32})
    {
        INFO(encoders << " encoders");
        const auto pages = ParameterPager::pages(groups, encoders);
        std::vector<int> order;
        for (const auto& page : pages)
        {
            CHECK_FALSE(page.parameters.empty());
            CHECK(static_cast<int>(page.parameters.size()) <= encoders);
            CHECK(page.name.isNotEmpty());
            order.insert(order.end(), page.parameters.begin(), page.parameters.end());
        }
        CHECK(order == expectedOrder);
    }
}

TEST_CASE("ParameterPager: small groups share a page, big ones split, none are broken up needlessly",
          "[devices][framework]")
{
    const auto pages = ParameterPager::pages(groupsOfSizes({3, 4, 2, 10}), 8);
    REQUIRE(pages.size() == 4);
    CHECK(pages[0].name == "G1 / G2"); // 3 + 4 fit together
    CHECK(pages[0].parameters.size() == 7);
    CHECK(pages[1].name == "G3"); // 2 more would not fit with G1 and G2
    CHECK(pages[2].name == "G4 1");
    CHECK(pages[2].parameters.size() == 8);
    CHECK(pages[3].name == "G4 2");
    CHECK(pages[3].parameters.size() == 2);

    const auto wide = ParameterPager::pages(groupsOfSizes({3, 4, 2, 10}), 32);
    REQUIRE(wide.size() == 1);
    CHECK(wide[0].name == "G1 / G2 / G3 / G4");
}

TEST_CASE("Every device type has unique, valid parameter IDs", "[devices][framework]")
{
    TrackTuning tuning;
    DeviceCreateContext context;
    context.tuning = &tuning;
    for (const auto& type : testRegistry().types())
    {
        INFO(type.id);
        const auto device = testRegistry().create(type.id, context);
        REQUIRE(device != nullptr);
        CHECK(device->typeId() == type.id);
        CHECK(device->kind() == type.kind);
        std::set<juce::String> seen;
        for (int i = 0; i < device->numParameters(); ++i)
        {
            const auto& id = device->parameter(i).id();
            CHECK(ParameterSpec::isValidId(id));
            CHECK(seen.insert(id).second);
            CHECK(device->parameterIndex(id) == i);
        }
        // Groups cover every parameter exactly once.
        std::size_t grouped = 0;
        for (const auto& g : device->parameterGroups())
            grouped += g.parameters.size();
        CHECK(grouped == static_cast<std::size_t>(device->numParameters()));
    }
}

TEST_CASE("ParameterSpec formats and parses values with their units", "[devices][framework]")
{
    const auto cutoff = ParameterSpec::continuous("cutoff", "Cutoff", 20.0f, 20000.0f, 1000.0f, ParamUnit::Hz);
    CHECK(cutoff.format(440.0f) == "440 Hz");
    CHECK(cutoff.format(2500.0f) == "2.5 kHz");
    CHECK(cutoff.parse("2.5k") == Catch::Approx(2500.0f));
    CHECK(cutoff.parse("  300 Hz") == Catch::Approx(300.0f));
    CHECK(cutoff.parse("99999") == Catch::Approx(20000.0f)); // constrained
    CHECK(cutoff.parse("loud") == Catch::Approx(1000.0f));   // unreadable: the default

    const auto gain = ParameterSpec::continuous("gain", "Gain", -70.0f, 6.0f, 0.0f, ParamUnit::Decibels);
    CHECK(gain.format(-70.0f) == "-inf dB");
    CHECK(gain.format(-6.0f) == "-6 dB");
    CHECK(gain.parse("-inf") == Catch::Approx(-70.0f));

    const auto mode = ParameterSpec::choice("mode", "Mode", {"Classic", "One-Shot", "Slice"}, 0);
    CHECK(mode.format(1.0f) == "One-Shot");
    CHECK(mode.parse("slice") == Catch::Approx(2.0f));

    const auto steps = ParameterSpec::integer("steps", "Steps", -24, 24, 0, ParamUnit::TableSteps);
    CHECK(steps.format(3.0f) == "+3 steps");
    CHECK(steps.parse("+3 steps") == Catch::Approx(3.0f));
    CHECK(steps.constrain(2.6f) == Catch::Approx(3.0f));

    CHECK(ParameterSpec::isValidId("osc1.shape"));
    CHECK_FALSE(ParameterSpec::isValidId("1osc"));
    CHECK_FALSE(ParameterSpec::isValidId("osc shape"));
}

// --- Device state in the document ---------------------------------------------------------------------

TEST_CASE("Parameter changes reach the device without rebuilding the snapshot", "[devices][framework]")
{
    Fixture f;
    const auto gainNode = f.add(TestGainDevice::typeId);
    auto* gain = f.instance<TestGainDevice>(gainNode);
    REQUIRE(gain != nullptr);
    CHECK(gain->parameter(0).plain() == Catch::Approx(1.0f));

    const auto generation = f.session.snapshots().generation();
    f.set().setDeviceParameter(gainNode, "gain", 0.25);
    f.session.flush();
    CHECK(gain->parameter(0).plain() == Catch::Approx(0.25f));
    CHECK(f.session.snapshots().generation() == generation);
    CHECK_FALSE(f.session.snapshots().isDirty());

    f.set().setDeviceEnabled(gainNode, false);
    CHECK_FALSE(gain->isEnabled());
    CHECK(f.session.snapshots().generation() == generation);

    // Undo goes the same way.
    f.session.undoManager().undo();
    CHECK(gain->isEnabled());
}

TEST_CASE("Dragging a knob is one undo step", "[devices][framework]")
{
    Fixture f;
    const auto gainNode = f.add(TestGainDevice::typeId);
    f.set().setDeviceParameter(gainNode, "gain", 0.5);
    f.session.undoManager().beginNewTransaction("Drag");
    for (int i = 1; i <= 20; ++i)
        f.set().setDeviceParameter(gainNode, "gain", 0.5 + i * 0.05);
    f.session.undoManager().beginNewTransaction();
    CHECK(LiveSet::deviceParameter(gainNode, "gain", 1.0) == Catch::Approx(1.5));
    f.session.undoManager().undo();
    CHECK(LiveSet::deviceParameter(gainNode, "gain", 1.0) == Catch::Approx(0.5));
    CHECK(f.instance<TestGainDevice>(gainNode)->parameter(0).plain() == Catch::Approx(0.5f));
}

TEST_CASE("Device values survive saving and loading the set exactly", "[devices][framework]")
{
    const auto file = juce::File::createTempFile(".lset");
    {
        Fixture f;
        auto synthNode = f.add(TestSynthDevice::typeId);
        auto gainNode = f.add(TestGainDevice::typeId);
        auto transposeNode = f.add(TestTransposeDevice::typeId, 0, 0);
        f.set().setDeviceParameter(synthNode, "sawMix", 0.123456789);
        f.set().setDeviceParameter(gainNode, "gain", -0.75);
        f.set().setDeviceParameter(transposeNode, "amount", -7);
        f.set().setDeviceEnabled(gainNode, false);
        REQUIRE(f.session.saveSet(file).wasOk());
    }

    Fixture g;
    REQUIRE(g.session.loadSet(file).wasOk());
    g.session.flush();
    const auto chain = g.chain();
    REQUIRE(chain.getNumChildren() == 3);
    CHECK(chain.getChild(0)[ids::deviceType].toString() == TestTransposeDevice::typeId);
    auto* transpose = g.instance<TestTransposeDevice>(chain.getChild(0));
    auto* synth = g.instance<TestSynthDevice>(chain.getChild(1));
    auto* gain = g.instance<TestGainDevice>(chain.getChild(2));
    REQUIRE(transpose != nullptr);
    REQUIRE(synth != nullptr);
    REQUIRE(gain != nullptr);
    CHECK(transpose->parameter(0).plain() == -7.0f);
    CHECK(synth->parameter(0).plain() == static_cast<float>(0.123456789));
    CHECK(gain->parameter(0).plain() == -0.75f);
    CHECK_FALSE(gain->isEnabled());
    CHECK(transpose->isEnabled());
    file.deleteFile();
}

TEST_CASE("A device's state round-trips through getStateInformation", "[devices][framework]")
{
    TestGainDevice a;
    a.parameter(0).setBase(0.3f);
    juce::ValueTree state("STATE");
    state.setProperty("sample", "kick.wav", nullptr);
    a.setState(state);
    juce::MemoryBlock block;
    a.getStateInformation(block);

    TestGainDevice b;
    b.setStateInformation(block.getData(), static_cast<int>(block.getSize()));
    CHECK(b.parameter(0).plain() == 0.3f);
    CHECK(b.state()["sample"].toString() == "kick.wav");
}

TEST_CASE("M2 sets get the default instrument on every track", "[devices][framework]")
{
    LiveSet set;
    set.resetToDefault(3, 2);
    auto xml = set.toXml();
    xml->setAttribute(ids::formatVersion.toString(), 1);
    for (auto* trackXml : xml->getChildByName(ids::tracks.toString())->getChildIterator())
        trackXml->deleteAllChildElementsWithTagName(ids::devices.toString());

    LiveSet loaded;
    REQUIRE(loaded.loadFromXml(*xml).wasOk());
    REQUIRE(loaded.numTracks() == 3);
    std::set<juce::int64> deviceIds;
    for (int t = 0; t < loaded.numTracks(); ++t)
    {
        const auto chain = LiveSet::chainOf(loaded.track(t));
        REQUIRE(chain.getNumChildren() == 1);
        CHECK(chain.getChild(0)[ids::deviceType].toString() == LiveSet::defaultInstrumentType());
        deviceIds.insert(static_cast<juce::int64>(chain.getChild(0)[ids::id]));
    }
    CHECK(deviceIds.size() == 3); // each got its own new ID
}

TEST_CASE("An unknown device type is kept and passes audio through", "[devices][framework]")
{
    Fixture f;
    f.add(TestSineDevice::typeId);
    auto unknown = f.add("future.device");
    f.set().setDeviceParameter(unknown, "mystery", 0.5);
    auto* missing = dynamic_cast<MissingDevice*>(f.session.snapshots().deviceFor(LiveSet::idOf(unknown)));
    REQUIRE(missing != nullptr);

    f.renderer.attach(f.session);
    f.renderer.captureAudio = true;
    f.renderer.renderTo(4800);
    CHECK(peak(f.renderer.audioLeft) > 0.2f); // 0.5 through two constant-power pans

    const auto xml = f.set().toXml();
    LiveSet reloaded;
    REQUIRE(reloaded.loadFromXml(*xml).wasOk());
    const auto kept = LiveSet::chainOf(reloaded.track(0)).getChild(1);
    CHECK(kept[ids::deviceType].toString() == "future.device");
    CHECK(LiveSet::deviceParameter(kept, "mystery", 0.0) == Catch::Approx(0.5));
}

// --- The on/off switch ---------------------------------------------------------------------------------------

TEST_CASE("Switching an audio effect off and on crossfades without a click", "[devices][framework]")
{
    for (const auto pattern : {BlockPattern::Fixed64, BlockPattern::Random})
    {
        INFO(describe(pattern));
        Fixture f(1, pattern);
        f.add(TestSineDevice::typeId);
        auto flip = f.add(TestGainDevice::typeId);
        f.set().setDeviceParameter(flip, "gain", -1.0); // flipping polarity: a hard switch jumps by 2x
        f.renderer.captureAudio = true;
        f.renderer.renderTo(9600);

        f.set().setDeviceEnabled(flip, false);
        f.renderer.renderTo(19200);
        f.set().setDeviceEnabled(flip, true);
        f.renderer.renderTo(28800);

        const auto& out = f.renderer.audioLeft;
        const float level = peak(out);
        REQUIRE(level > 0.2f); // 0.5 through two constant-power pans
        // A 100 Hz sine moves by at most level * 2pi * 100 / 48000 (0.013 * level) per sample; the
        // 5 ms crossfade adds at most 2 * level / 240.
        CHECK(largestStep(out, 4800) < 0.03f * level);
        // The switch did happen: off, the signal is the sine itself; on, it is flipped again.
        CHECK(out[19199 - 4] * out[28800 - 4] < 0.0f);
    }
}

TEST_CASE("A latency-adding effect switched off keeps its delay, so nothing shifts", "[devices][framework]")
{
    Fixture f;
    f.add(TestSineDevice::typeId);
    auto delay = f.add(TestLatencyDevice::typeId);
    f.renderer.captureAudio = true;
    f.renderer.renderTo(9600);
    f.set().setDeviceEnabled(delay, false);
    f.renderer.renderTo(19200);
    f.set().setDeviceEnabled(delay, true);
    f.renderer.renderTo(28800);
    // The dry path is delayed by the same 100 samples: the output is one continuous sine.
    CHECK(largestStep(f.renderer.audioLeft, 4800) < 0.02f * peak(f.renderer.audioLeft));
    CHECK(f.processor()->chain()->latencySamples == TestLatencyDevice::latency);
}

TEST_CASE("Switching an instrument off fades it out; on again, it fades back in", "[devices][framework]")
{
    Fixture f;
    auto sine = f.add(TestSineDevice::typeId);
    f.renderer.captureAudio = true;
    f.renderer.renderTo(9600);
    f.set().setDeviceEnabled(sine, false);
    f.renderer.renderTo(19200);
    const std::vector<float> silent(f.renderer.audioLeft.begin() + 9600 + 480, f.renderer.audioLeft.end());
    CHECK(peak(silent) == 0.0f);
    f.set().setDeviceEnabled(sine, true);
    f.renderer.renderTo(28800);
    CHECK(largestStep(f.renderer.audioLeft, 4800) < 0.03f * peak(f.renderer.audioLeft));
    CHECK(std::abs(f.renderer.audioLeft.back()) + std::abs(f.renderer.audioLeft[28800 - 60]) > 0.0f);
}

TEST_CASE("Switching a MIDI effect off ends the notes it was playing", "[devices][framework]")
{
    Fixture f;
    auto transpose = f.add(TestTransposeDevice::typeId);
    auto sineNode = f.add(TestSineDevice::typeId);
    auto* sine = f.instance<TestSineDevice>(sineNode);
    REQUIRE(sine != nullptr);
    addClip(f.set(), 0, 0, 8.0, {NoteData{60, 0.0, 4.0, 100, false}});
    f.session.launchSlot(0, 0);
    f.renderer.renderToBeat(1.5);
    CHECK(sine->held[72].load() == 1); // transposed up 12

    f.set().setDeviceEnabled(transpose, false);
    f.renderer.renderToBeat(2.0);
    CHECK(sine->held[72].load() == 0); // its note-off came at the switch
    f.renderer.renderToBeat(6.0);
    for (int n = 0; n < 128; ++n)
        CHECK(sine->held[static_cast<std::size_t>(n)].load() == 0);
}

// --- Chain changes ---------------------------------------------------------------------------------------------

TEST_CASE("Inserting, removing and reordering devices while playing never clicks", "[devices][framework]")
{
    for (const auto pattern : {BlockPattern::Fixed64, BlockPattern::Fixed512, BlockPattern::Random})
    {
        INFO(describe(pattern));
        Fixture f(1, pattern);
        f.add(TestSineDevice::typeId);
        f.renderer.captureAudio = true;
        f.renderer.renderTo(9600);

        auto flip = f.add(TestGainDevice::typeId); // inserted while playing
        f.set().setDeviceParameter(flip, "gain", -1.0);
        f.renderer.renderTo(19200);
        auto half = f.add(TestGainDevice::typeId, 0, 1);
        f.set().setDeviceParameter(half, "gain", 0.5);
        f.renderer.renderTo(28800);
        f.set().moveDevice(half, f.chain(), 2); // reordered
        f.session.flush();
        f.renderer.renderTo(38400);
        f.set().removeDevice(flip);
        f.session.flush();
        f.renderer.renderTo(48000);

        const auto& out = f.renderer.audioLeft;
        CHECK(largestStep(out, 4800) < 0.03f * peak(out));
        CHECK(out.back() * out[19200 - 1] != 0.0f);
    }
}

TEST_CASE("Devices that stay in the chain keep their instance; unchanged rebuilds publish nothing",
          "[devices][framework]")
{
    Fixture f;
    auto sineNode = f.add(TestSineDevice::typeId);
    auto* before = f.instance<TestSineDevice>(sineNode);
    auto chainBefore = f.processor()->chain();
    f.add(TestGainDevice::typeId);
    CHECK(f.instance<TestSineDevice>(sineNode) == before);
    CHECK(f.processor()->chain() != chainBefore);

    const auto chainNow = f.processor()->chain();
    f.set().track(0).setProperty(ids::name, "Renamed", nullptr);
    f.session.flush();
    CHECK(f.processor()->chain() == chainNow);
}

TEST_CASE("A removed device lives until the audio thread has switched away from it", "[devices][framework]")
{
    Fixture f;
    f.add(TestSineDevice::typeId);
    auto gainNode = f.add(TestGainDevice::typeId);
    f.renderer.attach(f.session);
    f.renderer.renderTo(4800);

    std::weak_ptr<StudioDevice> weak;
    for (const auto& d : f.processor()->chain()->devices)
        if (d->typeId() == TestGainDevice::typeId)
            weak = d;
    REQUIRE_FALSE(weak.expired());

    f.set().removeDevice(gainNode);
    f.session.flush();
    f.session.snapshots().collectGarbage();
    CHECK_FALSE(weak.expired()); // the audio thread is still fading the old chain out

    f.renderer.renderFor(64); // starts the fade
    f.session.snapshots().collectGarbage();
    CHECK_FALSE(weak.expired());

    f.renderer.renderFor(4800); // the fade is long done and the new chain is in use
    f.session.snapshots().collectGarbage();
    CHECK(weak.expired());
    CHECK(f.processor()->chainVersions() == 1);
}

TEST_CASE("Changing the MIDI path releases notes held in the devices that stay", "[devices][framework]")
{
    Fixture f;
    auto sineNode = f.add(TestSineDevice::typeId);
    auto* sine = f.instance<TestSineDevice>(sineNode);
    addClip(f.set(), 0, 0, 8.0, {NoteData{60, 0.0, 4.0, 100, false}});
    f.session.launchSlot(0, 0);
    f.renderer.renderToBeat(1.0);
    CHECK(sine->held[60].load() == 1);

    f.add(TestGainDevice::typeId); // an audio effect: the MIDI path is the same
    f.renderer.renderToBeat(1.5);
    CHECK(sine->releases.load() == 0);
    CHECK(sine->held[60].load() == 1);

    f.add(TestTransposeDevice::typeId, 0, 0); // a MIDI effect in front: notes are released
    f.renderer.renderToBeat(2.0);
    CHECK(sine->releases.load() == 1);
    CHECK(sine->held[60].load() == 0);
}

TEST_CASE("Chain changes, switches and parameter edits never allocate on the audio thread",
          "[devices][framework][rt]")
{
    LATTICE_REQUIRE_ALLOCATION_HOOKS();
    Fixture f(2, BlockPattern::Random);
    for (int t = 0; t < 2; ++t)
    {
        f.add(TestTransposeDevice::typeId, t);
        f.add(TestSynthDevice::typeId, t);
        f.add(TestGainDevice::typeId, t);
        addClip(f.set(), t, 0, 4.0, {NoteData{48 + t, 0.0, 1.0, 100, false}, NoteData{55, 1.0, 2.0, 90, false}});
    }
    f.session.launchScene(0);
    juce::Random random(7);
    for (int step = 0; step < 200; ++step)
    {
        const int t = random.nextInt(2);
        auto chain = f.chain(t);
        switch (random.nextInt(5))
        {
        case 0:
            f.set().setDeviceParameter(chain.getChild(random.nextInt(chain.getNumChildren())), "gain",
                                       random.nextFloat() * 2.0 - 0.5);
            break;
        case 1:
            if (chain.getNumChildren() > 0)
            {
                auto d = chain.getChild(random.nextInt(chain.getNumChildren()));
                f.set().setDeviceEnabled(d, !static_cast<bool>(d[ids::enabled]));
            }
            break;
        case 2:
            if (chain.getNumChildren() < 6)
                f.set().insertDevice(chain, random.nextInt(chain.getNumChildren() + 1), TestGainDevice::typeId);
            break;
        case 3:
            if (chain.getNumChildren() > 2)
                f.set().removeDevice(chain.getChild(random.nextInt(chain.getNumChildren())));
            break;
        default:
            if (chain.getNumChildren() > 1)
                f.set().moveDevice(chain.getChild(0), chain, chain.getNumChildren() - 1);
            break;
        }
        f.session.flush();
        f.renderer.renderFor(1 + random.nextInt(2000));
        f.session.snapshots().collectGarbage();
    }
    // Fixture checks the violation count.
}

// --- SettledShared --------------------------------------------------------------------------------------------

TEST_CASE("SettledShared frees only versions older than the one the reader settled on", "[core][framework]")
{
    SettledShared<int> shared;
    CHECK(shared.latest() == nullptr);
    shared.publish(std::make_shared<int>(1));
    const auto* v1 = shared.latest();
    shared.settle(v1);
    shared.publish(std::make_shared<int>(2));
    shared.publish(std::make_shared<int>(3));
    CHECK(shared.versionCount() == 3); // the reader may still be on 1

    shared.settle(shared.latest()); // jumped straight to 3
    shared.collect();
    CHECK(shared.versionCount() == 1);
    CHECK(*shared.published() == 3);

    shared.publish(std::make_shared<int>(4));
    shared.resetReader(); // the reader stopped: nothing older than the latest is in use
    CHECK(shared.versionCount() == 1);
    CHECK(*shared.latest()->object == 4);
}

// --- Real-time fuzz over every built-in device ------------------------------------------------------------

TEST_CASE("Real-time fuzz: every built-in device under random edits stays finite and never allocates",
          "[devices][framework][rt][fuzz]")
{
    LATTICE_REQUIRE_ALLOCATION_HOOKS();
    Fixture f(2, BlockPattern::Random);
    std::vector<juce::String> types;
    for (const auto& t : DeviceRegistry::builtin().types())
        types.push_back(t.id);
    REQUIRE(types.size() >= 4);

    // A sample for any Sampler that turns up.
    std::vector<float> wave(48000);
    for (std::size_t i = 0; i < wave.size(); ++i)
        wave[i] = 0.5f * std::sin(static_cast<float>(i) * 0.05f) * std::exp(-static_cast<float>(i % 12000) / 3000.0f);
    const SamplePtr sample = SampleData::fromChannels({wave}, 44100.0, "fuzz");
    const auto giveSamples = [&] {
        for (int t = 0; t < 2; ++t)
            for (const auto& d : f.chain(t))
                if (auto* sampler = f.instance<SamplerDevice>(d); sampler != nullptr && sampler->currentSample() == nullptr)
                    sampler->setSample(sample);
    };

    for (int t = 0; t < 2; ++t)
    {
        f.add(ArpeggiatorDevice::typeId, t);
        f.add(t == 0 ? SynthDevice::typeId : SamplerDevice::typeId, t);
        addClip(f.set(), t, 0, 4.0,
                {NoteData{48 + t, 0.0, 1.0, 100, false}, NoteData{55, 1.0, 2.0, 90, false}, NoteData{60, 2.5, 1.0, 70, false}});
    }
    giveSamples();
    f.renderer.captureAudio = true;
    f.session.launchScene(0);

    juce::Random random(2026);
    for (int step = 0; step < 400; ++step)
    {
        const int t = random.nextInt(2);
        auto chain = f.chain(t);
        const int action = random.nextInt(10);
        if (action < 5 && chain.getNumChildren() > 0)
        {
            // Several parameters of one device, anywhere in their ranges.
            auto node = chain.getChild(random.nextInt(chain.getNumChildren()));
            if (auto* device = f.session.snapshots().deviceFor(LiveSet::idOf(node)); device != nullptr && device->numParameters() > 0)
                for (int k = 0; k < 4; ++k)
                {
                    const auto& spec = device->parameter(random.nextInt(device->numParameters())).spec();
                    f.set().setDeviceParameter(node, spec.id, spec.minValue + random.nextDouble() * (spec.maxValue - spec.minValue));
                }
        }
        else if (action == 5 && chain.getNumChildren() < 7)
        {
            f.set().insertDevice(chain, random.nextInt(chain.getNumChildren() + 1),
                                 types[static_cast<std::size_t>(random.nextInt(static_cast<int>(types.size())))]);
        }
        else if (action == 6 && chain.getNumChildren() > 1)
        {
            f.set().removeDevice(chain.getChild(random.nextInt(chain.getNumChildren())));
        }
        else if (action == 7 && chain.getNumChildren() > 1)
        {
            f.set().moveDevice(chain.getChild(random.nextInt(chain.getNumChildren())), chain,
                               random.nextInt(chain.getNumChildren()));
        }
        else if (chain.getNumChildren() > 0)
        {
            auto d = chain.getChild(random.nextInt(chain.getNumChildren()));
            f.set().setDeviceEnabled(d, !static_cast<bool>(d[ids::enabled]));
        }
        f.session.flush();
        giveSamples();
        f.renderer.renderFor(1 + random.nextInt(3000));
        f.session.snapshots().collectGarbage();
        if (f.renderer.audioLeft.size() > 200000)
        {
            for (const float v : f.renderer.audioLeft)
                REQUIRE(std::isfinite(v));
            f.renderer.audioLeft.clear();
            f.renderer.audioRight.clear();
        }
    }
    for (const float v : f.renderer.audioLeft)
        REQUIRE(std::isfinite(v));
    // Fixture checks the violation count.
}
