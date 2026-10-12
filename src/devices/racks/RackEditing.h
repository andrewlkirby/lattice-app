#pragma once

#include "core/model/LiveSet.h"

#include <juce_data_structures/juce_data_structures.h>

#include <vector>

namespace lattice::racks
{

/** Editing racks in the document (docs/04-m3-devices.md, "3d"). Each operation goes through the
    set's undo manager; callers group them into transactions.

        DEVICE (a rack)
         ├─ PARAMS     macro1 .. macro64 (0..127)
         ├─ STATE      macroCount, tunedPads; MACRO children (name, colour); VARIATION children
         ├─ CHAINS / CHAIN   id, name, volumeDb, pan, mute, solo, key and velocity zones,
         │                   receiveNote, playNote, chokeGroup
         │                └─ DEVICES / DEVICE ...   (racks nest up to 4 deep)
         └─ MAPPINGS / MAPPING   macro, targetDevice, parameter, minimum, maximum, curve */

/** Inserts a rack of the given type (RackDevice::instrumentRackId, ...) into a chain. */
juce::ValueTree insertRack(LiveSet& set, juce::ValueTree chain, int index, const juce::String& typeId);

/** Adds a chain to a rack (at the end) and returns it. Its DEVICES is empty. */
juce::ValueTree addChain(LiveSet& set, juce::ValueTree rack, const juce::String& name = {});
void removeChain(LiveSet& set, const juce::ValueTree& chain);
/** The chain's DEVICES node. */
juce::ValueTree devicesOf(const juce::ValueTree& chain);
void setChainMix(LiveSet& set, juce::ValueTree chain, double volumeDb, double pan, bool mute, bool solo);
void setKeyZone(LiveSet& set, juce::ValueTree chain, int low, int high, int fadeLow = 0, int fadeHigh = 0);
void setVelocityZone(LiveSet& set, juce::ValueTree chain, int low, int high, int fadeLow = 0, int fadeHigh = 0);

/** Drum Rack: a pad on `note` holding a Sampler in One-Shot mode with fixed (untuned) pitch,
    playing `samplePath` (may be empty). Returns the pad's chain. */
juce::ValueTree addDrumPad(LiveSet& set, juce::ValueTree drumRack, int note, const juce::String& samplePath = {});
/** The pad chain for a note, or an invalid tree. */
juce::ValueTree padFor(const juce::ValueTree& drumRack, int note);
void setChokeGroup(LiveSet& set, juce::ValueTree pad, int group);
void setTunedPads(LiveSet& set, juce::ValueTree drumRack, bool tuned);

// --- Macros ---------------------------------------------------------------------------------------
int macroCount(const juce::ValueTree& rack);
/** 0..64. Lowering it removes the mappings of the macros that go (undo brings them back). */
void setMacroCount(LiveSet& set, juce::ValueTree rack, int count);
void setMacroName(LiveSet& set, juce::ValueTree rack, int macro, const juce::String& name);
/** The macro's name, or "Macro N" if it has none. */
juce::String macroName(const juce::ValueTree& rack, int macro);
/** The MAPPING nodes of one macro, or of every macro (macro = -1). */
std::vector<juce::ValueTree> mappingsOf(const juce::ValueTree& rack, int macro = -1);
/** Maps a macro (0-based) to a parameter of a device inside the rack, over [minimum, maximum] of
    the parameter's normalised range. Returns the MAPPING node. */
juce::ValueTree addMapping(LiveSet& set, juce::ValueTree rack, int macro, const juce::ValueTree& device,
                           const juce::String& parameterId, double minimum = 0.0, double maximum = 1.0,
                           double curve = 0.0);
void removeMapping(LiveSet& set, const juce::ValueTree& mapping);
int numMappings(const juce::ValueTree& rack);

/** Stores the current macro values as a variation; returns its index. */
int storeVariation(LiveSet& set, juce::ValueTree rack, const juce::String& name);
/** Sets every macro to the values stored in a variation. */
void recallVariation(LiveSet& set, juce::ValueTree rack, int index);
int numVariations(const juce::ValueTree& rack);
juce::String variationName(const juce::ValueTree& rack, int index);

} // namespace lattice::racks
