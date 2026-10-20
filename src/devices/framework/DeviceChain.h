#pragma once

#include "devices/framework/StudioDevice.h"

#include <memory>
#include <vector>

namespace lattice
{

struct RackLayout;

/** One version of a device chain, immutable once published (docs/04-m3-devices.md, "Chain
    changes"). Devices that stay in the chain from one version to the next are the same instances,
    so their sound continues. */
struct DeviceChainLayout
{
    std::vector<DevicePtr> devices; // in processing order: MIDI effects, instrument, audio effects
    /** For each device that is a rack, its structure (chains, macro mappings); null otherwise. The
        whole tree switches together, so editing inside a rack fades like any chain change. */
    std::vector<std::shared_ptr<RackLayout>> racks;
    /** For each device, its sidechain source (trackId 0 = none). */
    std::vector<SidechainRoute> sidechains;
    int latencySamples = 0;          // sum of the devices' latencies (racks: their slowest chain)

    /** The devices MIDI goes through (MIDI effects, instruments, racks), in order. A change here
        means held notes could be left hanging, so the chain releases them when it switches. */
    std::vector<const StudioDevice*> midiPath;

    /** Message thread, once every device is prepared: fills latencySamples and midiPath, and
        finalises the racks inside (their latencies and compensation delays). */
    void finalise();

    /** Calls fn(device) for every device in the tree, racks' chains included. */
    template <typename F>
    void forEachDevice(F&& fn) const;
};

/** Runs a DeviceChainLayout over one block (audio thread). Owns the scratch MIDI buffer the MIDI
    effects write into; sized in prepare() (message thread). Used by tracks and rack chains. */
class ChainRunner
{
public:
    void prepare(int maxEvents);

    /** audio: the chain's stereo buffer (cleared by the caller: a MIDI track's input is silence).
        input: the chain's MIDI input; MIDI effects may overwrite it. preFxTap, if given, receives
        the audio after the last instrument (the "pre-effects" sidechain point). Returns the number
        of MIDI events that were dropped because a buffer was full. */
    std::uint32_t process(const DeviceChainLayout& chain, float* const* audio, int numSamples, MidiBlock& input,
                          const DeviceTimeInfo& time, float* const* preFxTap = nullptr) noexcept LATTICE_NONBLOCKING;

private:
    std::vector<TimedMidi> scratchStorage;
};

} // namespace lattice
