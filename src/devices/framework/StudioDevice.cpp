#include "devices/framework/StudioDevice.h"

#include <algorithm>

namespace lattice
{

StudioDevice::StudioDevice(juce::String typeId, juce::String displayName, DeviceKind kind,
                           std::vector<ParameterSpec> parameterSpecs)
    : juce::AudioProcessor(BusesProperties().withOutput("Output", juce::AudioChannelSet::stereo(), true)),
      deviceType(std::move(typeId)),
      name(std::move(displayName)),
      deviceKind(kind),
      stateCopy("STATE")
{
    params.reserve(parameterSpecs.size());
    for (auto& spec : parameterSpecs)
    {
        jassert(ParameterSpec::isValidId(spec.id));
        jassert(parameterIndex(spec.id) < 0); // IDs are unique within a device
        const int index = static_cast<int>(params.size());
        const juce::String groupName = spec.group;
        auto parameter = std::make_unique<DeviceParameter>(std::move(spec), index);
        params.push_back(parameter.get());
        addParameter(parameter.release());

        auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.name == groupName; });
        if (it == groups.end())
        {
            groups.push_back({groupName, {}});
            it = groups.end() - 1;
        }
        it->parameters.push_back(index);
    }
}

StudioDevice::~StudioDevice() = default;

int StudioDevice::parameterIndex(const juce::String& parameterId) const noexcept
{
    for (std::size_t i = 0; i < params.size(); ++i)
        if (params[i]->id() == parameterId)
            return static_cast<int>(i);
    return -1;
}

DeviceParameter* StudioDevice::findParameter(const juce::String& parameterId) const noexcept
{
    const int i = parameterIndex(parameterId);
    return i >= 0 ? params[static_cast<std::size_t>(i)] : nullptr;
}

std::vector<ParameterGroup> StudioDevice::customPages(int) const
{
    return {};
}

void StudioDevice::prepareDevice(double newSampleRate, int maxBlockSize)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    maxBlockSize = std::max(maxBlockSize, 1);
    currentSampleRate = newSampleRate;
    setRateAndBufferSizeDetails(newSampleRate, maxBlockSize);
    prepare(newSampleRate, maxBlockSize);

    onMixStep = static_cast<float>(1.0 / std::max(1.0, bypassFadeSeconds * newSampleRate));
    onMix = isEnabled() ? 1.0f : 0.0f;
    midiEffectWasOn = isEnabled();
    dryBuffer.setSize(2, maxBlockSize, false, true, false);
    dryBuffer.clear();
    const int latency = latencySamples();
    dryDelay.setSize(2, std::max(latency, 1), false, true, false);
    dryDelay.clear();
    dryDelayPos = 0;
    heldOutput.fill(0);

    hostEventsIn.assign(4096, TimedMidi{});
    hostEventsOut.assign(4096, TimedMidi{});
    preparedBlock = maxBlockSize;
}

void StudioDevice::setState(const juce::ValueTree& newState)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    stateCopy = newState.isValid() ? newState.createCopy() : juce::ValueTree("STATE");
    stateChanged(stateCopy);
}

void StudioDevice::stateChanged(const juce::ValueTree&)
{
}

void StudioDevice::prepareToPlay(double newSampleRate, int maximumExpectedSamplesPerBlock)
{
    prepareDevice(newSampleRate, maximumExpectedSamplesPerBlock);
}

// --- Processing in a chain ------------------------------------------------------------------------------

void StudioDevice::processInChain(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept
{
    io.numSamples = std::min(io.numSamples, preparedBlock);
    if (io.numSamples <= 0)
        return;

    const float target = isEnabled() ? 1.0f : 0.0f;
    switch (switchKind())
    {
    case DeviceKind::MidiEffect:
        processMidiEffect(io, time);
        break;
    case DeviceKind::Instrument:
        processInstrument(io, time, target);
        break;
    case DeviceKind::AudioEffect:
    case DeviceKind::Rack:
        processAudioEffect(io, time, target);
        break;
    }
}

void StudioDevice::delayDry(const DeviceBuffers& io) noexcept
{
    const int n = io.numSamples;
    const int latency = latencySamples();
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* in = io.channels[std::min(ch, io.numChannels - 1)];
        float* out = dryBuffer.getWritePointer(ch);
        if (latency == 0)
        {
            std::copy_n(in, n, out);
            continue;
        }
        float* ring = dryDelay.getWritePointer(ch);
        int pos = dryDelayPos;
        for (int i = 0; i < n; ++i)
        {
            out[i] = ring[pos];
            ring[pos] = in[i];
            if (++pos == latency)
                pos = 0;
        }
    }
    if (latency > 0)
        dryDelayPos = static_cast<int>((dryDelayPos + n) % latency);
}

void StudioDevice::processAudioEffect(DeviceBuffers& io, const DeviceTimeInfo& time, float target) noexcept
{
    const int n = io.numSamples;
    const bool hasLatency = latencySamples() > 0;

    if (onMix == 0.0f && target == 0.0f)
    {
        // Fully off: pass the signal through, delayed like the processed signal would be.
        if (hasLatency)
        {
            delayDry(io);
            for (int ch = 0; ch < io.numChannels; ++ch)
                std::copy_n(dryBuffer.getReadPointer(std::min(ch, 1)), n, io.channels[ch]);
        }
        return;
    }
    if (onMix == 0.0f)
        reset(); // coming back on: forget the state from before it was switched off

    const bool fading = onMix != target;
    if (fading || hasLatency)
        delayDry(io);

    process(io, time);

    if (!fading)
        return;
    float m = onMix;
    const float step = target > onMix ? onMixStep : -onMixStep;
    for (int i = 0; i < n; ++i)
    {
        m = juce::jlimit(0.0f, 1.0f, m + step);
        for (int ch = 0; ch < io.numChannels; ++ch)
        {
            float* out = io.channels[ch];
            const float d = dryBuffer.getSample(std::min(ch, 1), i);
            out[i] = d + m * (out[i] - d);
        }
    }
    onMix = m;
}

void StudioDevice::processInstrument(DeviceBuffers& io, const DeviceTimeInfo& time, float target) noexcept
{
    const int n = io.numSamples;
    if (onMix == 0.0f && target == 0.0f)
    {
        for (int ch = 0; ch < io.numChannels; ++ch)
            std::fill_n(io.channels[ch], n, 0.0f);
        return;
    }
    if (onMix == 0.0f)
        reset();

    process(io, time);

    if (onMix == target)
        return;
    float m = onMix;
    const float step = target > onMix ? onMixStep : -onMixStep;
    for (int i = 0; i < n; ++i)
    {
        m = juce::jlimit(0.0f, 1.0f, m + step);
        for (int ch = 0; ch < io.numChannels; ++ch)
            io.channels[ch][i] *= m;
    }
    onMix = m;
}

void StudioDevice::processMidiEffect(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept
{
    jassert(io.midiIn != nullptr && io.midiOut != nullptr);
    if (io.midiIn == nullptr || io.midiOut == nullptr)
        return;

    const bool on = isEnabled();
    if (on != midiEffectWasOn)
    {
        // Whatever this device was playing ends now; then it starts afresh (or passes notes through).
        flushOutputNotes(*io.midiOut);
        reset();
        midiEffectWasOn = on;
    }

    if (on)
    {
        process(io, time);
    }
    else
    {
        for (const auto& e : *io.midiIn)
            io.midiOut->add(e);
    }
    trackOutputNotes(*io.midiOut);
}

void StudioDevice::trackOutputNotes(const MidiBlock& out) noexcept
{
    for (const auto& e : out)
    {
        const auto channelBit = static_cast<std::uint16_t>(1u << (e.bytes[0] & 0x0f));
        auto& held = heldOutput[e.bytes[1] & 0x7f];
        if (isNoteOn(e))
            held = static_cast<std::uint16_t>(held | channelBit);
        else if (isNoteOff(e))
            held = static_cast<std::uint16_t>(held & ~channelBit);
    }
}

void StudioDevice::flushOutputNotes(MidiBlock& out) noexcept
{
    for (int note = 0; note < 128; ++note)
    {
        auto& held = heldOutput[static_cast<std::size_t>(note)];
        for (int ch = 0; held != 0 && ch < 16; ++ch)
        {
            if ((held & (1u << ch)) != 0)
                out.add(0, static_cast<std::uint8_t>(0x80 | ch), static_cast<std::uint8_t>(note), 0);
        }
        held = 0;
    }
}

// --- juce::AudioProcessor ----------------------------------------------------------------------------------

void StudioDevice::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi)
{
    const int n = std::min(buffer.getNumSamples(), preparedBlock);
    MidiBlock in{hostEventsIn.data(), static_cast<int>(hostEventsIn.size()), 0, 0};
    MidiBlock out{hostEventsOut.data(), static_cast<int>(hostEventsOut.size()), 0, 0};
    for (const auto metadata : midi)
    {
        const auto message = metadata.getMessage();
        if (message.getRawDataSize() <= 3 && metadata.samplePosition < n)
        {
            TimedMidi e;
            e.sampleOffset = metadata.samplePosition;
            e.size = static_cast<std::uint8_t>(message.getRawDataSize());
            std::copy_n(message.getRawData(), e.size, e.bytes);
            in.add(e);
        }
    }

    std::array<float*, 2> channels{buffer.getWritePointer(0),
                                   buffer.getWritePointer(std::min(1, buffer.getNumChannels() - 1))};
    DeviceBuffers io;
    io.channels = channels.data();
    io.numChannels = std::min(2, buffer.getNumChannels());
    io.numSamples = n;
    io.midiIn = &in;
    io.midiOut = &out;
    DeviceTimeInfo time;
    time.sampleRate = currentSampleRate;
    processInChain(io, time);

    if (deviceKind == DeviceKind::MidiEffect)
    {
        midi.clear();
        for (const auto& e : out)
            midi.addEvent(e.bytes, e.size, e.sampleOffset);
    }
}

void StudioDevice::getStateInformation(juce::MemoryBlock& destination)
{
    juce::ValueTree root("DEVICE");
    root.setProperty("type", deviceType, nullptr);
    juce::ValueTree values("PARAMS");
    for (const auto* p : params)
        values.setProperty(p->id(), p->base(), nullptr);
    root.appendChild(values, nullptr);
    root.appendChild(stateCopy.createCopy(), nullptr);
    if (const auto xml = root.createXml())
        copyXmlToBinary(*xml, destination);
}

void StudioDevice::setStateInformation(const void* data, int sizeInBytes)
{
    const auto xml = getXmlFromBinary(data, sizeInBytes);
    if (xml == nullptr)
        return;
    const auto root = juce::ValueTree::fromXml(*xml);
    const auto values = root.getChildWithName("PARAMS");
    for (auto* p : params)
        p->setBase(values.hasProperty(p->id()) ? static_cast<float>(values[p->id()]) : p->spec().defaultValue);
    setState(root.getChildWithName("STATE"));
}

} // namespace lattice
