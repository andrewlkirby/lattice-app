// Returns, sends, sidechain and delay compensation (docs/04-m3-devices.md, "Tests" 8 and 9).

#include "devices/effects/Compressor.h"
#include "devices/effects/Utility.h"
#include "support/AllocationHooks.h"
#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace lattice;
using namespace lattice::test;

namespace
{
/** Tracks with empty chains, all armed (notes come in as live MIDI); returns as the set has them. */
struct Fixture
{
    explicit Fixture(int numTracks) : renderer(engine, BlockPattern::Fixed64)
    {
        session.newSet(numTracks, 1);
        for (int t = 0; t < numTracks; ++t)
        {
            auto chain = LiveSet::chainOf(set().track(t));
            while (chain.getNumChildren() > 0)
                set().removeDevice(chain.getChild(0));
            set().setTrackArm(t, true, true);
        }
        session.flush();
        renderer.captureAudio = true;
        violationsBefore = rt::violationCount();
    }
    ~Fixture() { CHECK(rt::violationCount() == violationsBefore); }

    LiveSet& set() { return session.set(); }
    juce::ValueTree chainOfTrack(int t) { return LiveSet::chainOf(set().track(t)); }
    juce::ValueTree chainOfReturn(int r) { return LiveSet::chainOf(set().returnTrack(r)); }
    juce::ValueTree add(juce::ValueTree chain, const juce::String& type) { return set().insertDevice(chain, -1, type); }
    void note(int n) { engine.midiInput().push(juce::MidiMessage::noteOn(1, n, static_cast<juce::uint8>(100))); }
    void render(std::int64_t samples)
    {
        session.flush();
        renderer.renderFor(samples);
    }
    double rmsOfLast(std::size_t count) const
    {
        const auto& out = renderer.audioLeft;
        double s = 0.0;
        for (auto i = out.size() - count; i < out.size(); ++i)
            s += static_cast<double>(out[i]) * out[i];
        return std::sqrt(s / static_cast<double>(count));
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer;
    std::uint64_t violationsBefore = 0;
};

double toDb(double g)
{
    return 20.0 * std::log10(std::max(g, 1e-12));
}
} // namespace

TEST_CASE("Delay compensation: tracks of different latencies and their returns reach the master on one sample",
          "[engine][mixer][latency]")
{
    Fixture f(2);
    REQUIRE(f.set().numReturns() >= 1);
    // Track 1: a step at the note. Track 2: the same, 100 samples late. Return A: 100 samples late too.
    f.add(f.chainOfTrack(0), TestRecorderDevice::typeId);
    f.add(f.chainOfTrack(1), TestRecorderDevice::typeId);
    f.add(f.chainOfTrack(1), TestLatencyDevice::typeId);
    f.add(f.chainOfReturn(0), TestLatencyDevice::typeId);
    f.set().setSendLevel(0, 0, 0.0);
    f.set().setSendLevel(1, 0, 0.0);
    f.render(4800);
    f.renderer.audioLeft.clear();

    f.note(60);
    f.render(4800);
    // Four paths (two direct, two through the return), all 200 samples after the note: the output
    // steps straight to its full level.
    const auto& out = f.renderer.audioLeft;
    std::size_t first = 0;
    while (first < out.size() && out[first] == 0.0f)
        ++first;
    REQUIRE(first < out.size());
    CHECK(out[first] == Catch::Approx(out.back()).epsilon(1e-4));
    CHECK(out.back() > 0.0f);
    // The note arrived at the start of the first block (live input), so 200 samples on.
    CHECK(first == 200);
}

TEST_CASE("Sends: pre-fader sends ignore the fader, post-fader sends follow it", "[engine][mixer]")
{
    Fixture f(1);
    f.add(f.chainOfTrack(0), TestRecorderDevice::typeId);
    f.set().setTrackVolumeDb(0, LiveSet::minVolumeDb); // the track's own output is off
    f.set().setSendLevel(0, 0, 0.0);
    f.note(60);
    f.render(9600);
    CHECK(f.rmsOfLast(2400) == 0.0); // post-fader (the default): nothing reaches the return

    f.set().setReturnPreFader(0, true);
    f.render(9600);
    CHECK(f.rmsOfLast(2400) > 0.01); // pre-fader: the return hears the track
}

TEST_CASE("Solo in place: soloing a track keeps the returns audible", "[engine][mixer]")
{
    Fixture f(2);
    f.add(f.chainOfTrack(0), TestRecorderDevice::typeId);
    f.add(f.chainOfTrack(1), TestRecorderDevice::typeId);
    f.set().setTrackVolumeDb(0, LiveSet::minVolumeDb);
    f.set().setReturnPreFader(0, true);
    f.set().setSendLevel(0, 0, 0.0); // track 1 is only heard through return A (pre-fader)
    f.note(60);
    f.render(9600);
    const double both = f.rmsOfLast(2400);

    f.set().setTrackSolo(0, true, false); // solo track 1: track 2 goes, return A stays
    f.render(9600);
    const double soloed = f.rmsOfLast(2400);
    CHECK(soloed > 0.01);
    CHECK(soloed < both);

    f.set().setTrackSolo(0, false, false);
    f.set().setReturnSolo(0, true); // solo the return: only it is heard
    f.render(9600);
    CHECK(f.rmsOfLast(2400) == Catch::Approx(soloed).epsilon(1e-3));
}

TEST_CASE("Sidechain: a kick on one track ducks a compressor on another by the expected amount",
          "[engine][mixer][sidechain]")
{
    Fixture f(2);
    // Track 1, the "kick": a steady 0.25 while its note is held; its own output muted.
    f.add(f.chainOfTrack(0), TestRecorderDevice::typeId);
    f.set().setTrackMute(0, true);
    // Track 2: a steady sine through a compressor listening to track 1.
    f.add(f.chainOfTrack(1), TestSineDevice::typeId);
    auto comp = f.add(f.chainOfTrack(1), CompressorDevice::typeId);
    f.set().setDeviceParameter(comp, "comp.threshold", -20.0);
    f.set().setDeviceParameter(comp, "comp.ratio", 4.0);
    f.set().setDeviceParameter(comp, "comp.knee", 0.0);
    f.set().setDeviceParameter(comp, "comp.attack", 1.0);
    f.set().setDeviceParameter(comp, "comp.sidechain", 1.0);
    auto state = comp.getChildWithName(ids::deviceState);
    state.setProperty(ids::sidechainTrack, static_cast<juce::int64>(f.set().trackId(0)), nullptr);
    f.render(24000);
    const double open = f.rmsOfLast(9600);

    f.engine.midiInput().push(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(100)));
    f.render(24000);
    const double ducked = f.rmsOfLast(9600);
    // The sidechain sits at 0.25 (-12 dB): 8 dB over the threshold, so 6 dB of reduction at 4:1.
    CHECK(toDb(ducked / open) == Catch::Approx(-6.0).margin(0.5));
}

TEST_CASE("Sidechain: before or after the source's effects", "[engine][mixer][sidechain]")
{
    for (const bool preFx : {false, true})
    {
        INFO((preFx ? "before effects" : "after effects"));
        Fixture f(2);
        f.add(f.chainOfTrack(0), TestRecorderDevice::typeId);
        auto silence = f.add(f.chainOfTrack(0), UtilityDevice::typeId); // the source's effects mute it
        f.set().setDeviceParameter(silence, "utility.gain", -70.0);
        f.add(f.chainOfTrack(1), TestSineDevice::typeId);
        auto comp = f.add(f.chainOfTrack(1), CompressorDevice::typeId);
        f.set().setDeviceParameter(comp, "comp.threshold", -20.0);
        f.set().setDeviceParameter(comp, "comp.ratio", 4.0);
        f.set().setDeviceParameter(comp, "comp.sidechain", 1.0);
        auto state = comp.getChildWithName(ids::deviceState);
        state.setProperty(ids::sidechainTrack, static_cast<juce::int64>(f.set().trackId(0)), nullptr);
        state.setProperty(ids::sidechainPreFx, preFx, nullptr);
        f.render(24000);
        const double open = f.rmsOfLast(9600);
        f.note(60);
        f.render(24000);
        const double after = f.rmsOfLast(9600);
        if (preFx)
            CHECK(toDb(after / open) < -3.0); // it hears the instrument
        else
            CHECK(toDb(after / open) == Catch::Approx(0.0).margin(0.1)); // it hears silence
    }
}

TEST_CASE("Sidechain: a loop between two tracks still renders (the editor refuses to make one)",
          "[engine][mixer][sidechain]")
{
    Fixture f(2);
    for (int t = 0; t < 2; ++t)
    {
        f.add(f.chainOfTrack(t), TestSineDevice::typeId);
        auto comp = f.add(f.chainOfTrack(t), CompressorDevice::typeId);
        f.set().setDeviceParameter(comp, "comp.sidechain", 1.0);
        comp.getChildWithName(ids::deviceState)
            .setProperty(ids::sidechainTrack, static_cast<juce::int64>(f.set().trackId(1 - t)), nullptr);
    }
    f.render(9600);
    for (const float v : f.renderer.audioLeft)
        REQUIRE(std::isfinite(v));
    CHECK(f.rmsOfLast(2400) > 0.01);
}

TEST_CASE("Reduced latency when monitoring: armed tracks skip their compensation delay", "[engine][mixer][latency]")
{
    for (const bool reduced : {false, true})
    {
        INFO((reduced ? "reduced" : "compensated"));
        Fixture f(2);
        f.set().state().setProperty(ids::reducedLatencyWhenMonitoring, reduced, nullptr);
        f.set().setTrackArm(1, false, true); // track 2 only plays back
        f.add(f.chainOfTrack(0), TestRecorderDevice::typeId);
        f.add(f.chainOfTrack(1), TestSineDevice::typeId);
        f.add(f.chainOfTrack(1), TestLatencyDevice::typeId); // 100 samples
        f.set().setTrackVolumeDb(1, LiveSet::minVolumeDb);
        f.render(4800);
        f.renderer.audioLeft.clear();
        f.note(60);
        f.render(4800);
        std::size_t first = 0;
        while (first < f.renderer.audioLeft.size() && f.renderer.audioLeft[first] == 0.0f)
            ++first;
        // Live input on the armed track: 100 samples late to line up with track 2, or at once.
        CHECK(first == (reduced ? 0u : 100u));
    }
}
