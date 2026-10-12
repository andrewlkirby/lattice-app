#pragma once

#include "devices/test/TuningTestSynth.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <vector>

namespace lattice
{

/** The M1 TuningTestSynth as a juce::AudioProcessor: the instrument of every M2 track
    (docs/03-m2-engine-session.md, "Device interface"). Built-in devices (M3) and hosted plugins (M6)
    share this interface. Stereo out (the mono synth on both channels), MIDI in.

    processBlock never allocates or locks (MIDI is carried in a buffer sized in prepareToPlay). */
class TestSynthProcessor final : public juce::AudioProcessor
{
public:
    static constexpr int maxEventsPerBlock = 4096;

    explicit TestSynthProcessor(const TrackTuning& tuning);

    TuningTestSynth& synth() noexcept { return testSynth; }

    const juce::String getName() const override { return "Test Synth"; }
    void prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock) override;
    void releaseResources() override {}
    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override;
    using juce::AudioProcessor::processBlock;

    double getTailLengthSeconds() const override { return 0.5; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    juce::AudioProcessorEditor* createEditor() override { return nullptr; }
    bool hasEditor() const override { return false; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    void getStateInformation(juce::MemoryBlock&) override {}
    void setStateInformation(const void*, int) override {}

private:
    TuningTestSynth testSynth;
    std::vector<TimedMidi> events;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(TestSynthProcessor)
};

} // namespace lattice
