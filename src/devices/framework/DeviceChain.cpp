#include "devices/framework/DeviceChain.h"

#include "devices/racks/RackLayout.h"

namespace lattice
{

void DeviceChainLayout::finalise()
{
    racks.resize(devices.size());
    sidechains.resize(devices.size());
    latencySamples = 0;
    midiPath.clear();
    for (std::size_t i = 0; i < devices.size(); ++i)
    {
        const auto& d = devices[i];
        if (const auto& rack = racks[i])
        {
            rack->finalise();
            latencySamples += rack->latencySamples;
        }
        else
        {
            latencySamples += d->latencySamples();
        }
        if (d->kind() != DeviceKind::AudioEffect)
            midiPath.push_back(d.get());
    }
}

void ChainRunner::prepare(int maxEvents)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    scratchStorage.assign(static_cast<std::size_t>(std::max(maxEvents, 1)), TimedMidi{});
}

std::uint32_t ChainRunner::process(const DeviceChainLayout& chain, float* const* audio, int numSamples, MidiBlock& input,
                                   const DeviceTimeInfo& time, float* const* preFxTap) noexcept
{
    MidiBlock scratch{scratchStorage.data(), static_cast<int>(scratchStorage.size()), 0, 0};
    MidiBlock* in = &input;
    MidiBlock* out = &scratch;

    // The pre-effects tap: after the last device that makes sound from MIDI.
    std::size_t lastSource = chain.devices.size();
    for (std::size_t i = 0; i < chain.devices.size(); ++i)
        if (chain.devices[i]->switchKind() == DeviceKind::Instrument)
            lastSource = i;

    for (std::size_t i = 0; i < chain.devices.size(); ++i)
    {
        const auto& device = chain.devices[i];
        DeviceBuffers io;
        io.channels = audio;
        io.numChannels = 2;
        io.numSamples = numSamples;
        io.midiIn = in;
        const float* sidechain[2] = {nullptr, nullptr};
        if (i < chain.sidechains.size() && chain.sidechains[i].trackId != 0 && time.sidechains != nullptr)
            if (const auto* source = time.sidechains->channels(chain.sidechains[i]))
            {
                sidechain[0] = source[0] + time.sidechainOffset;
                sidechain[1] = source[1] + time.sidechainOffset;
                io.sidechain = sidechain;
            }
        if (device->kind() == DeviceKind::MidiEffect)
        {
            out->clear();
            io.midiOut = out;
            device->processInChain(io, time);
            std::swap(in, out);
        }
        else
        {
            device->processInChain(io, time);
        }
        if (preFxTap != nullptr && i == lastSource)
            for (int ch = 0; ch < 2; ++ch)
                std::copy_n(audio[ch], numSamples, preFxTap[ch]);
    }
    if (preFxTap != nullptr && lastSource == chain.devices.size()) // no instrument: the input is the tap
        for (int ch = 0; ch < 2; ++ch)
            std::copy_n(audio[ch], numSamples, preFxTap[ch]);
    const auto lost = input.dropped + scratch.dropped;
    input.dropped = 0;
    return lost;
}

} // namespace lattice
