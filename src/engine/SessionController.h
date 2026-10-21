#pragma once

#include "core/model/LiveSet.h"
#include "core/model/MidiImport.h"
#include "engine/AudioEngine.h"
#include "engine/SnapshotBuilder.h"
#include "tuning/TuningRegistry.h"

#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <vector>

namespace lattice
{

/** The message-thread side of the session (docs/03-m2-engine-session.md): owns the document and its
    undo history, keeps the engine's snapshot up to date, turns user actions into engine commands,
    and turns what the engine reports (recorded notes, scene tempos) back into document edits.

    Every surface (the debug UI now, the real UI in M4, the Virtual Push and Push in M5) drives the
    session through this one API, so they all behave the same. Message thread only. */
class SessionController final : private juce::Timer
{
public:
    static constexpr int timerHz = 30;

    SessionController(AudioEngine& engine, TuningRegistry& tunings,
                      const DeviceRegistry& devices = DeviceRegistry::builtin());
    ~SessionController() override;

    LiveSet& set() noexcept { return liveSet; }
    const LiveSet& set() const noexcept { return liveSet; }
    juce::UndoManager& undoManager() noexcept { return undo; }
    SnapshotBuilder& snapshots() noexcept { return builder; }
    /** The device types this session creates devices from. */
    const DeviceRegistry& deviceRegistry() const noexcept { return deviceTypes; }

    /** Publishes any pending document change to the engine now. */
    void flush() { builder.rebuildIfNeeded(); }

    // --- Engine commands --------------------------------------------------------------------------
    /** On a group track's slot: launches the clips in that row on every track in the group (and
        stops those whose slot is empty). Otherwise, as clicking a slot: launches its clip; on an
        empty slot, starts recording if the track is armed,
        otherwise presses the slot's stop button. Clicking a clip that is recording ends it. */
    void launchSlot(int trackIndex, int sceneIndex);
    /** Releasing a slot's launch button (Gate clips). */
    void releaseSlot(int trackIndex, int sceneIndex);
    void launchScene(int sceneIndex);
    void stopTrack(int trackIndex);
    void stopAllClips();
    void play();
    void continuePlayback();
    void stop();
    void togglePlayStop();
    /** Session record (F9): overdubs into clips playing on armed tracks; armed tracks with nothing
        playing start recording in the selected scene's slot (or the next empty slot below it). */
    void setSessionRecord(bool on, int selectedScene);
    bool isSessionRecordOn() const noexcept { return transport().sessionRecord; }
    void setRandomSeed(std::uint64_t seed);

    /** Tap tempo: the average of the last 4 intervals; a pause of more than 2 s starts over.
        nowSeconds is for tests; by default the current time is used. */
    void tapTempo(double nowSeconds = -1.0);

    // --- State for the UI -------------------------------------------------------------------------
    SlotState slotState(int trackIndex, int sceneIndex) const;
    TransportState transport() const noexcept { return engine.session().transportState(); }
    TrackPlayState trackState(int trackIndex) const;
    std::array<float, 2> takeTrackPeaks(int trackIndex);
    std::array<float, 2> takeMasterPeaks() { return engine.session().takeMasterPeaks(); }
    std::array<float, 2> takeReturnPeaks(int returnIndex) { return engine.session().takeReturnPeaks(returnIndex); }
    int lastNoteOn(int trackIndex) const;
    std::shared_ptr<TrackProcessor> processorForTrack(int trackIndex) const;

    /** Applies what the engine reported (the timer calls this; tests call it directly). */
    void processEngineEvents();

    // --- Files ------------------------------------------------------------------------------------
    void newSet(int numTracks = LiveSet::defaultTracks, int numScenes = LiveSet::defaultScenes);
    juce::Result saveSet(const juce::File& file);
    juce::Result loadSet(const juce::File& file);
    const juce::File& currentFile() const noexcept { return file; }
    /** True if the document changed since it was last saved or loaded. */
    bool hasUnsavedChanges() const noexcept;
    /** Treats the document as it is now as saved, and clears the undo history (after setting up a
        new set's defaults). */
    void markClean();

    MidiImportResult importMidiFile(const juce::File& midiFile, int trackIndex, int sceneIndex);

    /** Called after a different set was loaded or created (the UI rebuilds itself). */
    std::function<void()> onSetReplaced;

private:
    struct OpenNote
    {
        double position = 0.0;
        double beat = 0.0;
        int velocity = 100;
    };

    /** Notes being recorded into one clip. */
    struct Capture
    {
        std::map<int, OpenNote> open; // by pitch
        std::vector<juce::ValueTree> added;
    };

    void timerCallback() override;
    void send(const EngineCommand& command);
    void startRecording(int trackIndex, int sceneIndex);
    void closeNote(std::int64_t clipId, Capture& capture, int pitch, double endBeat);
    void finishCapture(std::int64_t clipId, double finalLength);
    void afterReplacingSet();
    std::int64_t trackIdOf(int trackIndex) const { return liveSet.trackId(trackIndex); }

    AudioEngine& engine;
    TuningRegistry& registry;
    const DeviceRegistry& deviceTypes;
    juce::UndoManager undo;
    LiveSet liveSet{&undo};
    SnapshotBuilder builder;
    juce::File file;
    juce::ValueTree savedState;

    std::map<std::int64_t, Capture> captures;   // by clip ID
    std::map<std::int64_t, bool> newRecordings; // clip ID -> open-ended; clips created to record into
    std::deque<double> taps;
};

} // namespace lattice
