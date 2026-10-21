// Live MIDI input per track (docs/05-m4-ui.md, 4b, "I/O"): a track hears its input when monitoring
// is In, or Auto while armed; never when Off, though an armed track still records; and only what
// comes from its input and channel.

#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"

#include <catch2/catch_test_macros.hpp>

using namespace lattice;
using namespace lattice::test;

namespace
{
/** Tracks with a recorder each (a steady 0.25 while a note is held), none armed. */
struct Fixture
{
    explicit Fixture(int numTracks) : renderer(engine, BlockPattern::Fixed64)
    {
        session.newSet(numTracks, 2);
        for (int t = 0; t < numTracks; ++t)
        {
            auto chain = LiveSet::chainOf(set().track(t));
            while (chain.getNumChildren() > 0)
                set().removeDevice(chain.getChild(0));
            set().insertDevice(chain, -1, TestRecorderDevice::typeId);
            set().setTrackArm(t, false, true);
        }
        session.flush();
        renderer.captureAudio = true;
    }
    LiveSet& set() { return session.set(); }
    void render(std::int64_t samples)
    {
        session.flush();
        renderer.renderFor(samples);
        session.processEngineEvents();
    }
    /** Which tracks a note (from this source, on this channel) sounds on: by each track's note-ons. */
    std::vector<int> hearing(std::uint8_t source = computerKeyboardSource, int channel = 1)
    {
        render(480);
        renderer.attach(session); // record each track's events (its processor may be new)
        std::vector<std::size_t> before;
        for (int t = 0; t < set().numTracks(); ++t)
            before.push_back(renderer.noteOns(set().trackId(t)).size());
        engine.midiInput().push(juce::MidiMessage::noteOn(channel, 60, static_cast<juce::uint8>(100)), source);
        render(480);
        engine.midiInput().push(juce::MidiMessage::noteOff(channel, 60), source);
        render(480);
        std::vector<int> out;
        for (int t = 0; t < set().numTracks(); ++t)
            if (renderer.noteOns(set().trackId(t)).size() > before[static_cast<std::size_t>(t)])
                out.push_back(t);
        return out;
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer;
};
} // namespace

TEST_CASE("Live input: monitoring decides which tracks hear it", "[engine][input]")
{
    Fixture f(3);
    CHECK(f.hearing().empty()); // Auto, nothing armed
    f.set().setTrackArm(0, true, false);
    CHECK(f.hearing() == std::vector<int>{0});
    f.set().setTrackMonitoring(1, Monitoring::In); // not armed, but always listening
    CHECK(f.hearing() == std::vector<int>{0, 1});
    f.set().setTrackMonitoring(0, Monitoring::Off); // armed, but not listening
    CHECK(f.hearing() == std::vector<int>{1});
}

TEST_CASE("Live input: a track hears only its input and channel", "[engine][input]")
{
    Fixture f(4);
    const auto keyboardOnly = LiveSet::computerKeyboardInput;
    const juce::String device = "test-device-id";
    const auto deviceSource = f.engine.midiSourceFor(device);
    CHECK(deviceSource != computerKeyboardSource);
    CHECK(f.engine.midiSourceFor(device) == deviceSource); // stable
    for (int t = 0; t < 4; ++t)
        f.set().setTrackMonitoring(t, Monitoring::In);
    f.set().setTrackMidiInput(1, keyboardOnly);
    f.set().setTrackMidiInput(2, device);
    f.set().setTrackMidiInput(3, device);
    f.set().setTrackMidiChannel(3, 2);

    CHECK(f.hearing(computerKeyboardSource, 1) == std::vector<int>{0, 1}); // all inputs, the keyboard
    CHECK(f.hearing(deviceSource, 1) == std::vector<int>{0, 2});           // all inputs, the device on any channel
    CHECK(f.hearing(deviceSource, 2) == std::vector<int>{0, 2, 3});        // and channel 2
}

TEST_CASE("Live input: an armed track with monitoring Off records what it doesn't play", "[engine][input]")
{
    Fixture f(1);
    f.set().setTrackArm(0, true, false);
    f.set().setTrackMonitoring(0, Monitoring::Off);
    f.set().setGlobalQuantization(Quantization::None);
    f.render(480);
    f.session.launchSlot(0, 0); // stopped and armed: the transport and the recording start now
    f.render(4800);
    f.engine.midiInput().push(juce::MidiMessage::noteOn(1, 64, static_cast<juce::uint8>(100)));
    f.render(4800);
    f.engine.midiInput().push(juce::MidiMessage::noteOff(1, 64));
    f.render(4800);
    CHECK(f.renderer.noteOns(f.set().trackId(0)).empty()); // not heard
    f.session.launchSlot(0, 0); // ends the recording
    f.render(48000 * 3);
    const auto notes = LiveSet::notesOf(f.set().clip(0, 0));
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].pitch == 64);
}
