// "Lattice Test Plugin" (docs/07-m6-plugins-external.md, "Tests" 1): a VST3 (and LV2 on Linux)
// built with JUCE, for the scan and hosting (6b). A sine synth: Level, and Latency, which it reports
// as its latency (so a host's delay compensation can follow a change). CC 20 sets Level as if from
// its own window, notifying the host from the audio thread, as VST3 plugins do. It's built
// only for the tests, and never copied into a system plugin folder.

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <cmath>

namespace
{
class TestPlugin final : public juce::AudioProcessor, private juce::AudioProcessorParameter::Listener
{
public:
    TestPlugin() : juce::AudioProcessor(BusesProperties().withOutput("Out", juce::AudioChannelSet::stereo(), true))
    {
        addParameter(level = new juce::AudioParameterFloat(juce::ParameterID{"level", 1}, "Level", 0.0f, 1.0f, 0.25f));
        addParameter(latency = new juce::AudioParameterInt(juce::ParameterID{"latency", 1}, "Latency", 0, 4000, 0));
        latency->addListener(this);
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
            if (message.isController() && message.getControllerNumber() == 20)
                level->setValueNotifyingHost(static_cast<float>(message.getControllerValue()) / 127.0f);
            else if (message.isNoteOn())
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
        juce::MemoryOutputStream out(destination, false);
        out.writeFloat(level->get());
        out.writeInt(latency->get());
    }
    void setStateInformation(const void* data, int size) override
    {
        if (size >= 4)
        {
            juce::MemoryInputStream in(data, static_cast<std::size_t>(size), false);
            *level = in.readFloat();
            if (size >= 8)
                *latency = in.readInt();
        }
    }

private:
    void parameterValueChanged(int, float) override { setLatencySamples(latency->get()); }
    void parameterGestureChanged(int, bool) override {}

    juce::AudioParameterFloat* level = nullptr;
    juce::AudioParameterInt* latency = nullptr;
    double rate = 48000.0;
    std::array<bool, 128> notes{};
    std::array<double, 128> phases{};
};
} // namespace

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new TestPlugin();
}
