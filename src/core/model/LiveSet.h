#pragma once

#include "core/model/SessionTypes.h"

#include <juce_data_structures/juce_data_structures.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace lattice
{

/** Identifiers of the project document (docs/03-m2-engine-session.md, part 2a). */
namespace ids
{
// Node types
inline const juce::Identifier liveSet{"LIVESET"};
inline const juce::Identifier tracks{"TRACKS"};
inline const juce::Identifier track{"TRACK"};
inline const juce::Identifier slots{"SLOTS"};
inline const juce::Identifier slot{"SLOT"};
inline const juce::Identifier clip{"CLIP"};
inline const juce::Identifier notes{"NOTES"};
inline const juce::Identifier note{"NOTE"};
inline const juce::Identifier scenes{"SCENES"};
inline const juce::Identifier scene{"SCENE"};
inline const juce::Identifier master{"MASTER"};
inline const juce::Identifier returns{"RETURNS"};
inline const juce::Identifier returnTrack{"RETURN"};
inline const juce::Identifier sends{"SENDS"};
inline const juce::Identifier send{"SEND"};
inline const juce::Identifier devices{"DEVICES"};
inline const juce::Identifier device{"DEVICE"};
inline const juce::Identifier params{"PARAMS"};
inline const juce::Identifier deviceState{"STATE"};
inline const juce::Identifier rackChains{"CHAINS"};
inline const juce::Identifier rackChain{"CHAIN"};
inline const juce::Identifier mappings{"MAPPINGS"};
inline const juce::Identifier mapping{"MAPPING"};

// Set
inline const juce::Identifier formatVersion{"formatVersion"};
inline const juce::Identifier nextId{"nextId"};
inline const juce::Identifier tempo{"tempo"};
inline const juce::Identifier timeSigNum{"timeSigNum"};
inline const juce::Identifier timeSigDen{"timeSigDen"};
inline const juce::Identifier globalQuantization{"globalQuantization"};
inline const juce::Identifier metronomeOn{"metronomeOn"};
inline const juce::Identifier metronomeVolumeDb{"metronomeVolumeDb"};
inline const juce::Identifier selectNextSceneOnLaunch{"selectNextSceneOnLaunch"};
inline const juce::Identifier fixedRecordBars{"fixedRecordBars"};
inline const juce::Identifier reducedLatencyWhenMonitoring{"reducedLatencyWhenMonitoring"};

// Shared
inline const juce::Identifier id{"id"};
inline const juce::Identifier name{"name"};
inline const juce::Identifier colour{"colour"};

// Track and master
inline const juce::Identifier volumeDb{"volumeDb"};
inline const juce::Identifier pan{"pan"};
inline const juce::Identifier mute{"mute"};
inline const juce::Identifier solo{"solo"};
inline const juce::Identifier arm{"arm"};
inline const juce::Identifier tuningId{"tuningId"};
inline const juce::Identifier noteOffset{"noteOffset"};
inline const juce::Identifier highNotePolicy{"highNotePolicy"};
inline const juce::Identifier trackKind{"kind"};  // "group" for a group track; absent for a MIDI track
inline const juce::Identifier groupId{"groupId"}; // the ID of the group track a track is in (0: none)
inline const juce::Identifier folded{"folded"};   // a group track shown folded (not undoable)

// Return track (also id, name, colour, volumeDb, pan, mute, solo) and send
inline const juce::Identifier preFader{"preFader"};
inline const juce::Identifier returnId{"returnId"};
inline const juce::Identifier levelDb{"levelDb"};

// Device sidechain (in a device's STATE)
inline const juce::Identifier sidechainTrack{"sidechainTrack"}; // a track ID, 0 = none
inline const juce::Identifier sidechainPreFx{"sidechainPreFx"};

// Slot
inline const juce::Identifier hasStopButton{"hasStopButton"};

// Clip
inline const juce::Identifier startMarker{"startMarker"};
inline const juce::Identifier loopStart{"loopStart"};
inline const juce::Identifier loopEnd{"loopEnd"};
inline const juce::Identifier looping{"looping"};
inline const juce::Identifier launchMode{"launchMode"};
inline const juce::Identifier quantization{"quantization"};
inline const juce::Identifier legato{"legato"};
inline const juce::Identifier followA{"followA"};
inline const juce::Identifier followB{"followB"};
inline const juce::Identifier followChanceA{"followChanceA"};
inline const juce::Identifier followTime{"followTime"};

// Note
inline const juce::Identifier pitch{"pitch"};
inline const juce::Identifier start{"start"};
inline const juce::Identifier length{"length"};
inline const juce::Identifier velocity{"velocity"};
inline const juce::Identifier muted{"muted"};

// Device
inline const juce::Identifier deviceType{"type"};
inline const juce::Identifier enabled{"enabled"};

// Rack chain (also volumeDb, pan, mute, solo, name)
inline const juce::Identifier keyLow{"keyLow"};
inline const juce::Identifier keyHigh{"keyHigh"};
inline const juce::Identifier keyFadeLow{"keyFadeLow"};
inline const juce::Identifier keyFadeHigh{"keyFadeHigh"};
inline const juce::Identifier velocityLow{"velocityLow"};
inline const juce::Identifier velocityHigh{"velocityHigh"};
inline const juce::Identifier velocityFadeLow{"velocityFadeLow"};
inline const juce::Identifier velocityFadeHigh{"velocityFadeHigh"};
inline const juce::Identifier receiveNote{"receiveNote"};
inline const juce::Identifier playNote{"playNote"};
inline const juce::Identifier chokeGroup{"chokeGroup"};

// Macro mapping
inline const juce::Identifier macro{"macro"};
inline const juce::Identifier targetDevice{"targetDevice"};
inline const juce::Identifier parameter{"parameter"};
inline const juce::Identifier minimum{"minimum"};
inline const juce::Identifier maximum{"maximum"};
inline const juce::Identifier curve{"curve"};

// Scene
inline const juce::Identifier sceneTempo{"sceneTempo"};           // 0 = none
inline const juce::Identifier sceneTimeSigNum{"sceneTimeSigNum"}; // 0 = none
inline const juce::Identifier sceneTimeSigDen{"sceneTimeSigDen"};
} // namespace ids

/** One MIDI note of a clip. Times are in beats (quarter notes) from the clip's start. */
struct NoteData
{
    int pitch = 60;
    double start = 0.0;
    double length = 1.0;
    int velocity = 100;
    bool muted = false;

    bool operator==(const NoteData&) const = default;
};

/** The project document: a juce::ValueTree with typed accessors and operations
    (docs/03-m2-engine-session.md, part 2a).

        LIVESET  tempo, time signature, global quantization, metronome, ...
         ├─ TRACKS / TRACK  id, name, colour, mixer values, tuning; kind and groupId for groups
         │     ├─ SLOTS / SLOT (one per scene)  hasStopButton, optional CLIP
         │     │                 └─ CLIP  loop, launch settings, follow actions
         │     │                       └─ NOTES / NOTE  pitch, start, length, velocity, muted
         │     ├─ DEVICES / DEVICE  id, type, enabled   (the device chain, in order)
         │     │              ├─ PARAMS  one property per parameter ID: its plain value
         │     │              └─ STATE   the device's own state (sample files, pads, ...)
         │     └─ SENDS / SEND  returnId, levelDb
         ├─ RETURNS / RETURN  id, name, colour, mixer values, preFader; DEVICES
         ├─ SCENES / SCENE  id, name, colour, optional tempo and time signature
         └─ MASTER  volumeDb, pan; DEVICES

    Message thread only. Every operation goes through the UndoManager given at construction
    (when there is one), except where a method says otherwise; callers group edits into
    transactions with UndoManager::beginNewTransaction(). Tracks, clips and scenes have IDs that
    never change and are never reused within a set. */
class LiveSet
{
public:
    /** 1: M2. 2: device chains (M3); tracks of older sets get the default instrument.
        3: return tracks, sends and the master's chain (older sets get none of the first two). */
    static constexpr int currentFormatVersion = 3;
    static constexpr int defaultReturns = 2;
    static constexpr int maxReturns = 12;
    static constexpr double minTempo = 20.0;
    static constexpr double maxTempo = 999.0;
    static constexpr double minVolumeDb = -70.0; // the fader's bottom: silence
    static constexpr double maxVolumeDb = 6.0;
    static constexpr double minLoopLength = 1.0 / 16.0; // beats
    static constexpr int defaultTracks = 4;
    static constexpr int defaultScenes = 8;

    /** The instrument new tracks get. Set once at startup (before any LiveSet is created) by
        whoever knows the devices; the model itself does not. */
    static juce::String& defaultInstrumentType();

    explicit LiveSet(juce::UndoManager* undoManager = nullptr);

    juce::ValueTree& state() noexcept { return root; }
    const juce::ValueTree& state() const noexcept { return root; }
    juce::UndoManager* undoManager() const noexcept { return undo; }

    // --- Whole document (none of these are undoable; each clears the undo history) -------------
    /** A new set with numTracks MIDI tracks (the first armed) and numScenes scenes. */
    void resetToDefault(int numTracks = defaultTracks, int numScenes = defaultScenes);
    juce::Result loadFromXml(const juce::XmlElement& xml);
    std::unique_ptr<juce::XmlElement> toXml() const;
    juce::Result saveToFile(const juce::File& file) const;
    juce::Result loadFromFile(const juce::File& file);

    // --- Set ---------------------------------------------------------------------------------
    double tempo() const;
    void setTempo(double bpm, bool undoable = true);
    int timeSigNumerator() const;
    int timeSigDenominator() const;
    void setTimeSignature(int numerator, int denominator, bool undoable = true);
    /** Length of one bar in beats (quarter notes): numerator × 4 / denominator. */
    double beatsPerBar() const;
    Quantization globalQuantization() const;
    void setGlobalQuantization(Quantization q);
    bool metronomeOn() const;
    void setMetronomeOn(bool on);
    double metronomeVolumeDb() const;
    void setMetronomeVolumeDb(double db);
    bool selectNextSceneOnLaunch() const;
    void setSelectNextSceneOnLaunch(bool on);
    /** Fixed recording length in bars (1, 2, 4, 8 or 16), or 0 for open-ended recording. */
    int fixedRecordBars() const;
    void setFixedRecordBars(int bars);

    // --- Tracks --------------------------------------------------------------------------------
    int numTracks() const;
    juce::ValueTree track(int index) const;
    std::int64_t trackId(int index) const;
    int trackIndexForId(std::int64_t id) const; // -1 if none

    /** Inserts a MIDI track (at the end when insertIndex is out of range). Returns its index. It
        joins the group it lands in (see "Group tracks" below). */
    int addTrack(int insertIndex = -1);
    /** Deletes a track; a group track goes with its members. A set always keeps a track that isn't
        a group, so deleting everything else is refused. */
    void deleteTrack(int index);
    /** Copies a track (with its clips, under new IDs) to just after it, a group with its members.
        Returns the copy's index. */
    int duplicateTrack(int index);
    /** Moves a track (a group with its members) to where track `to` is: before it when moving up,
        after it when moving down. It joins the group it lands in. */
    void moveTrack(int from, int to);

    // --- Group tracks (docs/05-m4-ui.md, 4b) ----------------------------------------------------------
    // A group track is a summing bus with its own audio effects and mixer; it has no instrument and
    // no clips. Its members follow it in the track list, so a group and everything in it are one
    // contiguous block, and groups nest. Adding or moving a track puts it in the group it lands in:
    // right after a group's own track means inside that group; otherwise it joins the group of the
    // track that follows it (none at the end of the list).
    static bool isGroup(const juce::ValueTree& track);
    bool isGroupTrack(int index) const;
    /** The index of the group track a track is in, or -1. */
    int parentGroupOf(int index) const;
    /** How many groups a track is inside (0 at the top). */
    int groupDepth(int index) const;
    /** One past the last track of the block that starts at `index` (index + 1 for a plain track). */
    int blockEnd(int index) const;
    /** The tracks directly in a group (not their members), in order. */
    std::vector<int> groupMembers(int groupIndex) const;
    /** The plain (non-group) tracks inside a group, at any depth, in order. */
    std::vector<int> groupLeaves(int groupIndex) const;
    /** Puts tracks first..last into a new group track inserted before them, as one undo step for
        the caller. The range must be whole blocks at one level (no group cut in two). Returns the
        group's index, or -1 if the range isn't one. */
    int groupTracks(int first, int last);
    /** Takes a group's members out of it (into its own group, if it's inside one) and deletes the
        group track with its devices. */
    void ungroup(int groupIndex);
    void setGroupFolded(int groupIndex, bool fold);

    void setTrackName(int index, const juce::String& name);
    void setTrackVolumeDb(int index, double db);
    void setTrackPan(int index, double pan); // -1 (left) .. 1 (right)
    void setTrackMute(int index, bool mute);
    /** With additive false (a plain click), turning solo on clears every other track's solo.
        additive (Ctrl+click) adds to the current set. The same rule applies to arm. */
    void setTrackSolo(int index, bool solo, bool additive);
    void setTrackArm(int index, bool arm, bool additive);
    void setTrackTuning(int index, const juce::String& tuningId);
    void setTrackNoteOffset(int index, int offset);
    void setTrackHighNotePolicy(int index, int policy);

    juce::ValueTree master() const;
    void setMasterVolumeDb(double db);
    void setMasterPan(double pan);

    // --- Return tracks and sends ---------------------------------------------------------------------
    int numReturns() const;
    juce::ValueTree returnTrack(int index) const;
    std::int64_t returnId(int index) const;
    int returnIndexForId(std::int64_t id) const; // -1 if none
    /** Adds a return track at the end (at most maxReturns). Returns its index, or -1. */
    int addReturn();
    /** Deletes a return and every track's send to it. */
    void deleteReturn(int index);
    void setReturnName(int index, const juce::String& name);
    void setReturnVolumeDb(int index, double db);
    void setReturnPan(int index, double pan);
    void setReturnMute(int index, bool mute);
    void setReturnSolo(int index, bool solo);
    /** Sends to this return are taken before the tracks' faders (true) or after them (false). */
    void setReturnPreFader(int index, bool pre);
    /** A track's send to a return, in dB (minVolumeDb = off). */
    double sendLevel(int trackIndex, int returnIndex) const;
    void setSendLevel(int trackIndex, int returnIndex, double db);

    // --- Scenes --------------------------------------------------------------------------------
    int numScenes() const;
    juce::ValueTree scene(int index) const;
    /** Inserts a scene (and a slot in every track). Returns its index. */
    int addScene(int insertIndex = -1);
    void deleteScene(int index);
    int duplicateScene(int index);
    void moveScene(int from, int to);
    void setSceneName(int index, const juce::String& name);
    /** 0 removes the scene's tempo. */
    void setSceneTempo(int index, double bpm);
    /** numerator 0 removes the scene's time signature. */
    void setSceneTimeSignature(int index, int numerator, int denominator);

    // --- Slots and clips -----------------------------------------------------------------------
    juce::ValueTree slot(int trackIndex, int sceneIndex) const;
    /** The slot's clip, or an invalid tree if the slot is empty. */
    juce::ValueTree clip(int trackIndex, int sceneIndex) const;
    bool hasStopButton(int trackIndex, int sceneIndex) const;
    void setHasStopButton(int trackIndex, int sceneIndex, bool has);

    /** Creates an empty, looping clip of lengthBeats (replacing any clip in the slot). */
    juce::ValueTree createClip(int trackIndex, int sceneIndex, double lengthBeats,
                               const juce::String& name = {}, bool undoable = true);
    void deleteClip(int trackIndex, int sceneIndex, bool undoable = true);
    /** Copies the clip into the next empty slot below it. Returns that scene index, or -1 if there
        is no empty slot below. */
    int duplicateClip(int trackIndex, int sceneIndex);
    /** Moves a clip, replacing whatever is at the destination. */
    void moveClip(int fromTrack, int fromScene, int toTrack, int toScene);

    /** Location (track, scene) of a clip by ID, or (-1, -1). */
    std::pair<int, int> findClip(std::int64_t clipId) const;
    juce::ValueTree clipById(std::int64_t clipId) const;
    static std::int64_t idOf(const juce::ValueTree& node);

    void setClipProperty(juce::ValueTree clip, const juce::Identifier& property, const juce::var& value,
                         bool undoable = true);
    void setClipLoop(juce::ValueTree clip, double loopStart, double loopEnd, bool undoable = true);

    // --- Notes -----------------------------------------------------------------------------------
    juce::ValueTree addNote(const juce::ValueTree& clip, const NoteData& note, bool undoable = true);
    void removeNote(const juce::ValueTree& clip, const juce::ValueTree& note, bool undoable = true);
    void setNote(juce::ValueTree note, const NoteData& data);
    static NoteData noteData(const juce::ValueTree& note);
    static std::vector<NoteData> notesOf(const juce::ValueTree& clip);

    // --- Devices ----------------------------------------------------------------------------------
    /** The device chain (DEVICES node) of a track (later also returns and the master). */
    static juce::ValueTree chainOf(const juce::ValueTree& owner);
    /** Inserts a new device of the given type at index (the end when out of range). The caller
        decides where it may go (see devices/ChainEditing.h). */
    juce::ValueTree insertDevice(juce::ValueTree chain, int index, const juce::String& typeId);
    /** Inserts a copy of a device tree (a preset, a pasted device) with new IDs. */
    juce::ValueTree insertDeviceCopy(juce::ValueTree chain, int index, const juce::ValueTree& device);
    void removeDevice(const juce::ValueTree& device);
    void moveDevice(const juce::ValueTree& device, juce::ValueTree toChain, int toIndex);
    void setDeviceEnabled(juce::ValueTree device, bool on);
    /** Sets a parameter's plain value. Consecutive changes of one parameter within one transaction
        become one undo step (dragging a knob). */
    void setDeviceParameter(juce::ValueTree device, const juce::String& parameterId, double value);
    /** The saved value, or `fallback` (the parameter's default) when none is saved. */
    static double deviceParameter(const juce::ValueTree& device, const juce::String& parameterId, double fallback);
    juce::ValueTree deviceById(std::int64_t id) const;
    /** The TRACK (or other chain owner) a device belongs to, or an invalid tree. */
    static juce::ValueTree chainOwnerOf(const juce::ValueTree& node);
    /** A new DEVICE node (not yet in the document). */
    juce::ValueTree makeDevice(const juce::String& typeId);

    /** A new ID, unique within this set. Not undoable (IDs are never reused). */
    std::int64_t allocateId();

private:
    juce::UndoManager* um(bool undoable) const noexcept { return undoable ? undo : nullptr; }
    juce::ValueTree tracksNode() const;
    juce::ValueTree scenesNode() const;
    juce::ValueTree returnsNode() const;
    juce::ValueTree makeReturn(int number);
    juce::ValueTree makeTrack(int number);
    juce::ValueTree makeScene();
    /** The group a track inserted at insertIndex would join (0: none). */
    juce::int64 groupAt(int insertIndex) const;
    juce::ValueTree makeSlot() const;
    /** Gives every TRACK, CLIP and SCENE in tree a new ID. */
    void reassignIds(const juce::ValueTree& tree);
    void setExclusiveFlag(int index, const juce::Identifier& flag, bool on, bool additive);

    juce::ValueTree root;
    juce::UndoManager* undo = nullptr;
};

} // namespace lattice
