#include "engine/SessionController.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <utility>

namespace lattice
{
namespace
{
/** Undo/redo of a recording into a new clip: the whole clip comes and goes. */
class RecordClipAction final : public juce::UndoableAction
{
public:
    RecordClipAction(juce::ValueTree slotNode, juce::ValueTree clipNode)
        : slot(std::move(slotNode)), clip(std::move(clipNode))
    {
    }

    bool perform() override
    {
        if (firstTime)
        {
            firstTime = false; // already in the document
            return true;
        }
        if (!clip.getParent().isValid() && !slot.getChildWithName(ids::clip).isValid())
            slot.appendChild(clip, nullptr);
        return true;
    }

    bool undo() override
    {
        if (clip.getParent() == slot)
            slot.removeChild(clip, nullptr);
        return true;
    }

private:
    juce::ValueTree slot;
    juce::ValueTree clip;
    bool firstTime = true;
};

/** Undo/redo of notes overdubbed into an existing clip. */
class OverdubAction final : public juce::UndoableAction
{
public:
    OverdubAction(juce::ValueTree notesNode, std::vector<juce::ValueTree> addedNotes)
        : notes(std::move(notesNode)), added(std::move(addedNotes))
    {
    }

    bool perform() override
    {
        if (firstTime)
        {
            firstTime = false;
            return true;
        }
        for (auto& n : added)
            if (!n.getParent().isValid())
                notes.appendChild(n, nullptr);
        return true;
    }

    bool undo() override
    {
        for (auto& n : added)
            if (n.getParent() == notes)
                notes.removeChild(n, nullptr);
        return true;
    }

private:
    juce::ValueTree notes;
    std::vector<juce::ValueTree> added;
    bool firstTime = true;
};
} // namespace

SessionController::SessionController(AudioEngine& e, TuningRegistry& tunings, const DeviceRegistry& devices)
    : engine(e), registry(tunings), deviceTypes(devices), builder(
                                        liveSet, engine,
                                        [this](const juce::String& id) {
                                            auto t = registry.findById(id);
                                            return t != nullptr ? t : registry.defaultTuning();
                                        },
                                        devices)
{
    savedState = liveSet.state().createCopy();
    builder.rebuildNow();
    startTimerHz(timerHz);
}

SessionController::~SessionController()
{
    stopTimer();
}

void SessionController::timerCallback()
{
    processEngineEvents();
    builder.collectGarbage();
}

void SessionController::send(const EngineCommand& command)
{
    flush();
    const bool sent = engine.sendCommand(command);
    jassert(sent); // the command queue is full: the audio thread isn't running?
    juce::ignoreUnused(sent);
}

// --- Commands ---------------------------------------------------------------------------------------

bool SessionController::isAutoArmed(int trackIndex) const
{
    if (trackIndex < 0 || trackIndex >= liveSet.numTracks() || builder.autoArmTrack() == 0 ||
        trackIdOf(trackIndex) != builder.autoArmTrack() || liveSet.isGroupTrack(trackIndex))
        return false;
    for (int t = 0; t < liveSet.numTracks(); ++t)
        if (static_cast<bool>(liveSet.track(t)[ids::arm]))
            return false;
    return true;
}

bool SessionController::isArmed(int trackIndex) const
{
    return (trackIndex >= 0 && trackIndex < liveSet.numTracks() && static_cast<bool>(liveSet.track(trackIndex)[ids::arm])) ||
           isAutoArmed(trackIndex);
}

void SessionController::launchSlot(int trackIndex, int sceneIndex)
{
    if (trackIndex < 0 || trackIndex >= liveSet.numTracks() || sceneIndex < 0 ||
        sceneIndex >= liveSet.numScenes())
        return;
    flush();

    if (liveSet.isGroupTrack(trackIndex))
    {
        // A group's slot launches the clips in that row on every track in it, and stops the
        // tracks whose slot there is empty (as a scene would, for those tracks only). It never
        // starts a recording.
        for (const int leaf : liveSet.groupLeaves(trackIndex))
        {
            const auto clip = liveSet.clip(leaf, sceneIndex);
            EngineCommand c;
            c.type = CommandType::LaunchSlot;
            c.trackId = trackIdOf(leaf);
            c.scene = sceneIndex;
            c.clipId = clip.isValid() ? LiveSet::idOf(clip) : 0;
            send(c);
        }
        return;
    }

    const auto clip = liveSet.clip(trackIndex, sceneIndex);
    const auto state = trackState(trackIndex);
    EngineCommand c;
    c.trackId = trackIdOf(trackIndex);
    c.scene = sceneIndex;

    if (clip.isValid())
    {
        c.clipId = LiveSet::idOf(clip);
        const auto rec = newRecordings.find(c.clipId);
        const bool recordingHere =
            rec != newRecordings.end() && rec->second && state.playingClipId == c.clipId;
        const bool recordQueuedHere =
            state.queued == TrackPlayState::Queued::Record && state.queuedClipId == c.clipId;
        if (recordingHere || recordQueuedHere)
        {
            c.type = CommandType::EndRecording; // ends the recording (or cancels it before it starts)
            send(c);
            return;
        }
        c.type = CommandType::LaunchSlot;
        send(c);
        return;
    }

    if (isArmed(trackIndex))
    {
        startRecording(trackIndex, sceneIndex);
        return;
    }
    c.type = CommandType::LaunchSlot; // the slot's stop button
    send(c);
}

void SessionController::releaseSlot(int trackIndex, int sceneIndex)
{
    if (trackIndex >= 0 && trackIndex < liveSet.numTracks() && liveSet.isGroupTrack(trackIndex))
    {
        for (const int leaf : liveSet.groupLeaves(trackIndex))
            releaseSlot(leaf, sceneIndex);
        return;
    }
    const auto clip = liveSet.clip(trackIndex, sceneIndex);
    if (!clip.isValid())
        return;
    EngineCommand c;
    c.type = CommandType::ReleaseSlot;
    c.trackId = trackIdOf(trackIndex);
    c.scene = sceneIndex;
    c.clipId = LiveSet::idOf(clip);
    send(c);
}

void SessionController::launchScene(int sceneIndex)
{
    if (sceneIndex < 0 || sceneIndex >= liveSet.numScenes())
        return;
    EngineCommand c;
    c.type = CommandType::LaunchScene;
    c.scene = sceneIndex;
    send(c);
}

void SessionController::stopTrack(int trackIndex)
{
    if (trackIndex < 0 || trackIndex >= liveSet.numTracks())
        return;
    if (liveSet.isGroupTrack(trackIndex))
    {
        for (const int leaf : liveSet.groupLeaves(trackIndex))
            stopTrack(leaf);
        return;
    }
    EngineCommand c;
    c.type = CommandType::StopTrack;
    c.trackId = trackIdOf(trackIndex);
    send(c);
}

void SessionController::previewNote(int trackIndex, int pitch, int velocity)
{
    if (trackIndex < 0 || trackIndex >= liveSet.numTracks() || pitch < 0 || pitch > 127)
        return;
    const auto id = trackIdOf(trackIndex);
    const auto sounding = std::find(previews.begin(), previews.end(), std::pair{id, pitch});
    if (velocity <= 0 && sounding == previews.end())
        return;
    EngineCommand c;
    c.type = CommandType::PreviewNote;
    c.trackId = id;
    c.value = static_cast<std::uint64_t>(pitch);
    c.scene = juce::jlimit(0, 127, velocity);
    send(c);
    if (velocity <= 0)
        previews.erase(sounding);
    else if (sounding == previews.end())
        previews.emplace_back(id, pitch);
}

void SessionController::endPreviews()
{
    for (const auto& [id, pitch] : std::exchange(previews, {}))
    {
        EngineCommand c;
        c.type = CommandType::PreviewNote;
        c.trackId = id;
        c.value = static_cast<std::uint64_t>(pitch);
        c.scene = 0;
        send(c);
    }
}

void SessionController::stopAllClips()
{
    EngineCommand c;
    c.type = CommandType::StopAll;
    send(c);
}

void SessionController::play()
{
    EngineCommand c;
    c.type = CommandType::Play;
    send(c);
}

void SessionController::continuePlayback()
{
    EngineCommand c;
    c.type = CommandType::Continue;
    send(c);
}

void SessionController::stop()
{
    EngineCommand c;
    c.type = CommandType::Stop;
    send(c);
}

void SessionController::togglePlayStop()
{
    if (transport().playing)
        stop();
    else
        play();
}

void SessionController::setSessionRecord(bool on, int selectedScene)
{
    EngineCommand c;
    c.type = CommandType::SetSessionRecord;
    c.flag = on;
    send(c);
    if (!on)
        return;

    // Armed tracks with nothing playing start a new recording.
    for (int t = 0; t < liveSet.numTracks(); ++t)
    {
        if (!static_cast<bool>(liveSet.track(t)[ids::arm]))
            continue;
        const auto state = trackState(t);
        if (state.playingClipId != 0 || state.queued != TrackPlayState::Queued::None)
            continue;
        for (int s = std::max(0, selectedScene); s < liveSet.numScenes(); ++s)
        {
            if (!liveSet.clip(t, s).isValid())
            {
                startRecording(t, s);
                break;
            }
        }
    }
}

void SessionController::setRandomSeed(std::uint64_t seed)
{
    EngineCommand c;
    c.type = CommandType::SetRandomSeed;
    c.value = seed;
    send(c);
}

void SessionController::startRecording(int trackIndex, int sceneIndex)
{
    const int fixedBars = liveSet.fixedRecordBars();
    const bool openEnded = fixedBars == 0;
    const double length = (openEnded ? 1 : fixedBars) * liveSet.beatsPerBar();

    // The clip exists while it records; the undo step for it is added when the recording ends.
    const auto clip = liveSet.createClip(trackIndex, sceneIndex, length, {}, false);
    const auto clipId = LiveSet::idOf(clip);
    newRecordings[clipId] = openEnded;

    EngineCommand c;
    c.type = CommandType::StartRecording;
    c.trackId = trackIdOf(trackIndex);
    c.scene = sceneIndex;
    c.clipId = clipId;
    c.flag = openEnded;
    send(c);
}

void SessionController::tapTempo(double nowSeconds)
{
    if (nowSeconds < 0.0)
        nowSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
    if (!taps.empty() && nowSeconds - taps.back() > 2.0)
        taps.clear();
    taps.push_back(nowSeconds);
    while (taps.size() > 5)
        taps.pop_front();
    if (taps.size() < 2)
        return;

    const double average = (taps.back() - taps.front()) / static_cast<double>(taps.size() - 1);
    undo.beginNewTransaction("Tap tempo");
    liveSet.setTempo(std::round(60.0 / average * 100.0) / 100.0);
}

// --- State -------------------------------------------------------------------------------------------

std::shared_ptr<TrackProcessor> SessionController::processorForTrack(int trackIndex) const
{
    if (trackIndex < 0 || trackIndex >= liveSet.numTracks())
        return nullptr;
    return builder.processorFor(trackIdOf(trackIndex));
}

TrackPlayState SessionController::trackState(int trackIndex) const
{
    const auto p = processorForTrack(trackIndex);
    return p != nullptr ? p->playState() : TrackPlayState{};
}

std::array<float, 2> SessionController::takeTrackPeaks(int trackIndex)
{
    const auto p = processorForTrack(trackIndex);
    return p != nullptr ? p->takePeaks() : std::array<float, 2>{};
}

int SessionController::lastNoteOn(int trackIndex) const
{
    const auto p = processorForTrack(trackIndex);
    return p != nullptr ? p->lastNoteOn() : -1;
}

SlotState SessionController::slotState(int trackIndex, int sceneIndex) const
{
    if (trackIndex >= 0 && trackIndex < liveSet.numTracks() && liveSet.isGroupTrack(trackIndex))
    {
        // A group's slot shows the liveliest state of the slots in its row: recording, then
        // playing, then waiting to launch or stop, then holding a clip.
        constexpr SlotState order[] = {SlotState::Recording, SlotState::QueuedRecord, SlotState::Playing,
                                       SlotState::QueuedLaunch, SlotState::QueuedStop, SlotState::Stopped};
        int best = static_cast<int>(std::size(order));
        for (const int leaf : liveSet.groupLeaves(trackIndex))
        {
            const auto s = slotState(leaf, sceneIndex);
            for (int i = 0; i < best; ++i)
                if (order[i] == s)
                    best = i;
        }
        return best < static_cast<int>(std::size(order)) ? order[best] : SlotState::Empty;
    }
    const auto clip = liveSet.clip(trackIndex, sceneIndex);
    const auto state = trackState(trackIndex);
    const std::int64_t id = clip.isValid() ? LiveSet::idOf(clip) : 0;

    if (id != 0 && state.queued == TrackPlayState::Queued::Record && state.queuedClipId == id)
        return SlotState::QueuedRecord;
    if (id == 0)
        return SlotState::Empty;
    if (state.playingClipId == id)
    {
        if (state.recording)
            return SlotState::Recording;
        if (state.queued == TrackPlayState::Queued::Stop)
            return SlotState::QueuedStop;
        return SlotState::Playing;
    }
    if (state.queued == TrackPlayState::Queued::Launch && state.queuedClipId == id)
        return SlotState::QueuedLaunch;
    return SlotState::Stopped;
}

// --- Engine feedback ---------------------------------------------------------------------------------

void SessionController::closeNote(std::int64_t clipId, Capture& capture, int pitch, double endBeat)
{
    const auto it = capture.open.find(pitch);
    if (it == capture.open.end())
        return;
    const auto on = it->second;
    capture.open.erase(it);

    auto clip = liveSet.clipById(clipId);
    if (!clip.isValid())
        return;

    NoteData note;
    note.pitch = pitch;
    note.start = on.position;
    note.velocity = on.velocity;
    note.length = std::max(endBeat - on.beat, 1.0 / 128.0);
    if (!newRecordings.contains(clipId))
    {
        // Overdub: a note can't be longer than the loop.
        const double loopLength =
            static_cast<double>(clip[ids::loopEnd]) - static_cast<double>(clip[ids::loopStart]);
        note.length = std::min(note.length, std::max(loopLength, 1.0 / 128.0));
    }
    capture.added.push_back(liveSet.addNote(clip, note, false));
}

void SessionController::finishCapture(std::int64_t clipId, double finalLength)
{
    auto& capture = captures[clipId];
    for (auto it = capture.open.begin(); it != capture.open.end();)
    {
        const int pitch = it->first;
        const double beat = it->second.beat;
        it = std::next(it);
        closeNote(clipId, capture, pitch, beat + 1.0 / 128.0);
    }

    auto clip = liveSet.clipById(clipId);
    const auto rec = newRecordings.find(clipId);
    if (clip.isValid() && rec != newRecordings.end())
    {
        if (finalLength > 0.0)
        {
            liveSet.setClipLoop(clip, 0.0, finalLength, false);
            liveSet.setClipProperty(clip, ids::looping, true, false);
        }
        undo.beginNewTransaction("Record clip");
        undo.perform(new RecordClipAction(clip.getParent(), clip));
        newRecordings.erase(rec);
    }
    else if (clip.isValid() && !capture.added.empty())
    {
        undo.beginNewTransaction("Overdub");
        undo.perform(new OverdubAction(clip.getChildWithName(ids::notes), capture.added));
    }
    captures.erase(clipId);
}

void SessionController::processEngineEvents()
{
    auto& queue = engine.engineEvents();
    EngineEvent e;
    while (queue.pop(e))
    {
        switch (e.type)
        {
        case EngineEventType::CaptureNoteOn:
            captures[e.clipId].open[e.pitch] = {e.position, e.beat, e.velocity};
            break;
        case EngineEventType::CaptureNoteOff:
            closeNote(e.clipId, captures[e.clipId], e.pitch, e.beat);
            break;
        case EngineEventType::CaptureEnded:
            finishCapture(e.clipId, e.value);
            break;
        case EngineEventType::RecordingCancelled:
            if (newRecordings.erase(e.clipId) > 0)
            {
                const auto [t, s] = liveSet.findClip(e.clipId);
                if (t >= 0 && liveSet.clip(t, s).getChildWithName(ids::notes).getNumChildren() == 0)
                    liveSet.deleteClip(t, s, false);
            }
            captures.erase(e.clipId);
            break;
        case EngineEventType::TempoChanged:
            liveSet.setTempo(e.value, false);
            break;
        case EngineEventType::TimeSignatureChanged:
            liveSet.setTimeSignature(e.numerator, e.denominator, false);
            break;
        }
    }
    flush();
}

// --- Files -------------------------------------------------------------------------------------------

void SessionController::afterReplacingSet()
{
    captures.clear();
    newRecordings.clear();
    previews.clear(); // the tracks they sounded on are gone
    builder.forgetAll();
    builder.rebuildNow();
    savedState = liveSet.state().createCopy();
    if (onSetReplaced)
        onSetReplaced();
}

void SessionController::newSet(int numTracks, int numScenes)
{
    stop();
    liveSet.resetToDefault(numTracks, numScenes);
    file = juce::File();
    afterReplacingSet();
}

juce::Result SessionController::saveSet(const juce::File& target)
{
    const auto result = liveSet.saveToFile(target);
    if (result.wasOk())
    {
        file = target;
        savedState = liveSet.state().createCopy();
    }
    return result;
}

juce::Result SessionController::loadSet(const juce::File& source)
{
    stop();
    const auto result = liveSet.loadFromFile(source);
    if (result.failed())
        return result;
    file = source;
    afterReplacingSet();
    return result;
}

bool SessionController::hasUnsavedChanges() const noexcept
{
    return !liveSet.state().isEquivalentTo(savedState);
}

void SessionController::markClean()
{
    undo.clearUndoHistory();
    savedState = liveSet.state().createCopy();
}

MidiImportResult SessionController::importMidiFile(const juce::File& midiFile, int trackIndex, int sceneIndex)
{
    undo.beginNewTransaction("Import MIDI file");
    auto result = lattice::importMidiFile(liveSet, midiFile, trackIndex, sceneIndex);
    flush();
    return result;
}

} // namespace lattice
