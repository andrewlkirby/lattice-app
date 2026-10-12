#include "core/model/MidiImport.h"

#include <cmath>
#include <vector>

namespace lattice
{
namespace
{
struct ImportedTrack
{
    juce::String name;
    std::vector<NoteData> notes;
    double end = 0.0;
};

ImportedTrack readTrack(const juce::MidiMessageSequence& source, double ticksPerBeat)
{
    juce::MidiMessageSequence seq(source);
    seq.updateMatchedPairs();

    ImportedTrack result;
    for (int i = 0; i < seq.getNumEvents(); ++i)
    {
        const auto* event = seq.getEventPointer(i);
        const auto& m = event->message;
        if (m.isTrackNameEvent() && result.name.isEmpty())
            result.name = m.getTextFromTextMetaEvent();
        if (!m.isNoteOn())
            continue;

        NoteData note;
        note.pitch = m.getNoteNumber();
        note.velocity = juce::jlimit(1, 127, static_cast<int>(m.getVelocity()));
        note.start = m.getTimeStamp() / ticksPerBeat;
        const double endTicks = event->noteOffObject != nullptr ? event->noteOffObject->message.getTimeStamp()
                                                                : m.getTimeStamp() + ticksPerBeat / 4.0;
        note.length = std::max(endTicks / ticksPerBeat - note.start, 1.0 / 128.0);
        result.end = std::max(result.end, note.start + note.length);
        result.notes.push_back(note);
    }
    return result;
}
} // namespace

MidiImportResult importMidi(LiveSet& set, juce::InputStream& stream, const juce::String& clipName,
                            int trackIndex, int sceneIndex)
{
    MidiImportResult result;
    if (trackIndex < 0 || trackIndex >= set.numTracks() || sceneIndex < 0 || sceneIndex >= set.numScenes())
    {
        result.error = "There is no such slot.";
        return result;
    }

    juce::MidiFile file;
    if (!file.readFrom(stream))
    {
        result.error = "This is not a standard MIDI file.";
        return result;
    }
    const int timeFormat = file.getTimeFormat();
    if (timeFormat <= 0)
    {
        result.error = "MIDI files with SMPTE timing are not supported.";
        return result;
    }

    std::vector<ImportedTrack> tracks;
    for (int i = 0; i < file.getNumTracks(); ++i)
        if (auto t = readTrack(*file.getTrack(i), timeFormat); !t.notes.empty())
            tracks.push_back(std::move(t));

    if (tracks.empty())
    {
        result.error = "The MIDI file has no notes.";
        return result;
    }

    const double bar = set.beatsPerBar();
    for (std::size_t k = 0; k < tracks.size() && trackIndex + static_cast<int>(k) < set.numTracks(); ++k)
    {
        const auto& t = tracks[k];
        const double length = std::max(1.0, std::ceil(t.end / bar - 1.0e-9)) * bar;
        juce::String name = clipName;
        if (tracks.size() > 1 && t.name.isNotEmpty())
            name << " - " << t.name;

        auto clip = set.createClip(trackIndex + static_cast<int>(k), sceneIndex, length, name);
        for (const auto& note : t.notes)
            set.addNote(clip, note);
        ++result.clipsCreated;
    }
    return result;
}

MidiImportResult importMidiFile(LiveSet& set, const juce::File& file, int trackIndex, int sceneIndex)
{
    juce::FileInputStream stream(file);
    if (!stream.openedOk())
        return {0, "Could not open " + file.getFullPathName()};
    return importMidi(set, stream, file.getFileNameWithoutExtension(), trackIndex, sceneIndex);
}

} // namespace lattice
