#pragma once

#include "devices/framework/StudioDevice.h"

#include <memory>
#include <vector>

namespace lattice
{

/** One version of a device chain, immutable once published (docs/04-m3-devices.md, "Chain
    changes"). Devices that stay in the chain from one version to the next are the same instances,
    so their sound continues. */
struct DeviceChainLayout
{
    std::vector<DevicePtr> devices; // in processing order: MIDI effects, instrument, audio effects
    int latencySamples = 0;          // sum of the devices' latencies

    /** The devices MIDI goes through (MIDI effects, instruments, racks), in order. A change here
        means held notes could be left hanging, so the chain releases them when it switches. */
    std::vector<const StudioDevice*> midiPath;

    /** Fills latencySamples and midiPath from devices. */
    void finalise();
};

/** Runs a DeviceChainLayout over one block (audio thread). Owns the scratch MIDI buffer the MIDI
    effects write into; sized in prepare() (message thread). Used by tracks and rack chains. */
class ChainRunner
{
public:
    void prepare(int maxEvents);

    /** audio: the chain's stereo buffer (cleared by the caller: a MIDI track's input is silence).
        input: the chain's MIDI input; MIDI effects may overwrite it. Returns the number of MIDI
        events that were dropped because a buffer was full. */
    std::uint32_t process(const DeviceChainLayout& chain, float* const* audio, int numSamples, MidiBlock& input,
                 const DeviceTimeInfo& time) noexcept LATTICE_NONBLOCKING;

private:
    std::vector<TimedMidi> scratchStorage;
};

} // namespace lattice
