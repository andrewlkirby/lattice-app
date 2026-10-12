#pragma once

#include "core/model/LiveSet.h"

#include <juce_audio_basics/juce_audio_basics.h>

namespace lattice
{

struct MidiImportResult
{
    int clipsCreated = 0;
    juce::String error; // empty on success

    bool ok() const noexcept { return error.isEmpty(); }
};

/** Imports a standard MIDI file (docs/03-m2-engine-session.md, "MIDI import"). Each MIDI track
    that has notes becomes one clip: the first in (trackIndex, sceneIndex), the next in the track to
    its right, and so on, as long as there are tracks. Clip lengths round up to whole bars of the
    set's time signature. Every channel of a MIDI track is merged into its clip.

    Message thread. The edits go through the set's UndoManager; the caller starts the transaction. */
MidiImportResult importMidiFile(LiveSet& set, const juce::File& file, int trackIndex, int sceneIndex);

/** The same, from a stream. clipName names the clips. */
MidiImportResult importMidi(LiveSet& set, juce::InputStream& stream, const juce::String& clipName,
                            int trackIndex, int sceneIndex);

} // namespace lattice
