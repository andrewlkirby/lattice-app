// "Lattice Test Plugin" (docs/07-m6-plugins-external.md, "Tests" 1): a VST3 (and LV2 on Linux)
// built with JUCE, for the scan and, from 6b, hosting. A sine synth with one parameter. It's built
// only for the tests, and never copied into a system plugin folder.

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cmath>

namespace
{
class TestPlugin final : public juce::AudioProcessor
{
public:
    TestPlugin() : juce::AudioProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true))
    {
        addParameter(level = new juce::AudioParameterFloat(juce::ParameterID{"level", 1}, "Level", 0.0f, 1.0f, 0.25f));
    }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return true; }
    bool producesMidi() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }
    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram(int) override {}
    const juce::String getProgramName(int) override { return {}; }
    void changeProgramName(int, const juce::String&) override {}
    bool hasEditor() const override { return true; }
    juce::AudioProcessorEditor* createEditor() override { return new juce::GenericAudioProcessorEditor(*this); }

    void prepareToPlay(double sampleRate, int) override
    {
        rate = sampleRate;
        notes.fill(false);
    }
    void releaseResources() override {}

    void processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) override
    {
        for (const auto m : midi)
        {
            const auto message = m.getMessage();
            if (message.isNoteOn())
                notes[static_cast<std::size_t>(message.getNoteNumber())] = true;
            else if (message.isNoteOff())
                notes[static_cast<std::size_t>(message.getNoteNumber())] = false;
        }
        buffer.clear();
        const float gain = level->get();
        for (int note = 0; note < 128; ++note)
        {
            if (!notes[static_cast<std::size_t>(note)])
                continue;
            const double step = juce::MathConstants<double>::twoPi * juce::MidiMessage::getMidiNoteInHertz(note) / rate;
            auto& phase = phases[static_cast<std::size_t>(note)];
            for (int i = 0; i < buffer.getNumSamples(); ++i)
            {
                const auto s = gain * static_cast<float>(std::sin(phase));
                phase += step;
                for (int c = 0; c < buffer.getNumChannels(); ++c)
                    buffer.addSample(c, i, s);
            }
            phase = std::fmod(phase, juce::MathConstants<double>::twoPi);
        }
    }

    void getStateInformation(juce::MemoryBlock& destination) override
    {
        juce::MemoryOutputStream(destination, false).writeFloat(level->get());
    }
    void setStateInformation(const void* data, int size) override
    {
        if (size >= 4)
            *level = juce::MemoryInputStream(data, static_cast<std::size_t>(size), false).readFloat();
    }

private:
    juce::AudioParameterFloat* level = nullptr;
    double rate = 48000.0;
    std::array<bool, 128> notes{};
    std::array<double, 128> phases{};
};
} // namespace

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TestPlugin();
}
