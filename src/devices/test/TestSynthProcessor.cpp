#include "devices/test/TestSynthProcessor.h"

#include <algorithm>

namespace lattice
{

TestSynthProcessor::TestSynthProcessor(const TrackTuning& tuning)
    : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      testSynth(tuning)
{
}

void TestSynthProcessor::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    juce::ignoreUnused(maximumExpectedSamplesPerBlock);
    testSynth.prepare(sampleRate);
    events.assign(static_cast<std::size_t>(maxEventsPerBlock), TimedMidi{});
}

void TestSynthProcessor::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    const int numSamples = buffer.getNumSamples();
    int numEvents = 0;
    for (const auto metadata : midi)
    {
        if (numEvents >= static_cast<int>(events.size()) || metadata.numBytes < 1 || metadata.numBytes > 3)
            continue;
        auto& e = events[static_cast<std::size_t>(numEvents++)];
        e.sampleOffset = metadata.samplePosition;
        e.size = static_cast<std::uint8_t>(metadata.numBytes);
        std::copy_n(metadata.data, metadata.numBytes, e.bytes);
    }

    float* left = buffer.getWritePointer(0);
    testSynth.render(left, numSamples, events.data(), numEvents);
    for (int ch = 1; ch < buffer.getNumChannels(); ++ch)
        buffer.copyFrom(ch, 0, left, numSamples);
}

} // namespace lattice
