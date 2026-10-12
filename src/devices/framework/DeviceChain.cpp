#include "devices/framework/DeviceChain.h"

#include "devices/racks/RackLayout.h"

namespace lattice
{

void DeviceChainLayout::finalise()
{
    racks.resize(devices.size());
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
                          const DeviceTimeInfo& time) noexcept
{
    MidiBlock scratch{scratchStorage.data(), static_cast<int>(scratchStorage.size()), 0, 0};
    MidiBlock* in = &input;
    MidiBlock* out = &scratch;

    for (const auto& device : chain.devices)
    {
        DeviceBuffers io;
        io.channels = audio;
        io.numChannels = 2;
        io.numSamples = numSamples;
        io.midiIn = in;
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
    }
    const auto lost = input.dropped + scratch.dropped;
    input.dropped = 0;
    return lost;
}

} // namespace lattice
