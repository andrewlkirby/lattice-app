#pragma once

#include "core/model/LiveSet.h"
#include "devices/framework/DeviceRegistry.h"

#include <vector>

namespace lattice::chains
{

/** Where devices may go in a chain (docs/04-m3-devices.md, "3f"), as in Live:

    - a chain that carries MIDI (a track's, an Instrument or Drum Rack's chain) runs
      MIDI effects -> at most one instrument -> audio effects;
    - an audio-only chain (a group track's, a return's, the master's, an Audio Effect Rack's)
      holds audio effects only;
    - an Instrument or Drum Rack counts as an instrument, an Audio Effect Rack as an audio effect;
    - racks nest at most RackDevice::maxDepth deep, and never inside themselves.

    A new instrument where there already is one replaces it. Every edit goes through the set's
    undo manager; the caller groups them into one transaction. Message thread. */

enum class Role : std::uint8_t
{
    MidiEffect,
    Instrument,
    AudioEffect
};

Role roleOf(const juce::String& typeId, const DeviceRegistry& registry);

/** True for a track's chain and the chains of Instrument and Drum Racks. */
bool carriesMidi(const juce::ValueTree& chain);

/** How many racks a chain sits inside (0 for a track's own chain). */
int depthOf(const juce::ValueTree& chain);

struct Placement
{
    bool allowed = false;
    int index = -1;           // the device's index in the chain once it is there
    juce::ValueTree replaces; // the instrument it replaces, if any
    juce::String reason;      // why not, when not allowed
};

/** Where a device of this type would go if dropped at desiredIndex (-1: the end). `device` is the
    device being moved, if it is already in the document (its own place doesn't count). */
Placement place(const juce::ValueTree& chain, const juce::String& typeId, int desiredIndex,
                const DeviceRegistry& registry, const juce::ValueTree& device = {});

/** Inserts a new device by the rules. Returns it, or an invalid tree if it can't go there. */
juce::ValueTree insert(LiveSet& set, const juce::ValueTree& chain, const juce::String& typeId, int desiredIndex,
                       const DeviceRegistry& registry);
/** Inserts a copy of a device tree (a preset) by the rules. */
juce::ValueTree insertCopy(LiveSet& set, const juce::ValueTree& chain, const juce::ValueTree& device, int desiredIndex,
                           const DeviceRegistry& registry);
/** Moves a device (within its chain or to another) by the rules. Returns false if it can't go there. */
bool move(LiveSet& set, const juce::ValueTree& device, const juce::ValueTree& toChain, int desiredIndex,
          const DeviceRegistry& registry);

// --- Sidechain --------------------------------------------------------------------------------------
/** The tracks (by ID) a device may take its sidechain from: every track except its own and the
    ones that already listen to its own track, directly or through others (that would be a loop).
    A device on a return or the master may listen to any track. Group tracks are never sources. */
std::vector<std::int64_t> sidechainSources(const LiveSet& set, const juce::ValueTree& device);
/** Sets (or with trackId 0 clears) a device's sidechain source. Refuses a loop. */
bool setSidechainSource(LiveSet& set, juce::ValueTree device, std::int64_t trackId, bool preFx);

} // namespace lattice::chains
