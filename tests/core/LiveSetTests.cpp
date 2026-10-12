#include "core/model/LiveSet.h"
#include "core/model/MidiImport.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <set>

using namespace lattice;

namespace
{
/** The document as XML, without the ID counter (which undo deliberately never rewinds). */
juce::String documentText(const LiveSet& set)
{
    auto copy = set.state().createCopy();
    copy.removeProperty(ids::nextId, nullptr);
    return copy.toXmlString();
}

NoteData note(int pitch, double start, double length, int velocity = 100)
{
    NoteData n;
    n.pitch = pitch;
    n.start = start;
    n.length = length;
    n.velocity = velocity;
    return n;
}

/** A set with some of everything in it. */
void fill(LiveSet& set)
{
    set.setTempo(97.5);
    set.setTimeSignature(7, 8);
    set.setGlobalQuantization(Quantization::Half);
    set.setMetronomeOn(true);
    set.setFixedRecordBars(2);
    set.setTrackVolumeDb(1, -6.5);
    set.setTrackPan(1, 0.25);
    set.setTrackMute(2, true);
    set.setTrackTuning(0, "builtin.colundi");
    set.setTrackNoteOffset(0, -3);
    set.setSceneName(1, "Chorus");
    set.setSceneTempo(1, 140.0);
    set.setSceneTimeSignature(2, 3, 4);
    set.setHasStopButton(3, 5, false);
    set.setMasterVolumeDb(-1.5);
    auto clip = set.createClip(0, 0, 8.0, "Bass");
    set.addNote(clip, note(47, 0.0, 0.5, 90));
    set.addNote(clip, note(52, 1.25, 0.333333333333, 127));
    set.setClipLoop(clip, 1.0, 7.5);
    set.setClipProperty(clip, ids::followA, static_cast<int>(FollowAction::Next));
    set.setClipProperty(clip, ids::followChanceA, 62.5);
    set.setClipProperty(clip, ids::launchMode, static_cast<int>(LaunchMode::Gate));
    set.createClip(2, 3, 4.0, "Lead");
}
} // namespace

TEST_CASE("A new set has tracks, scenes and one slot per scene", "[core][model]")
{
    LiveSet set;
    CHECK(set.numTracks() == LiveSet::defaultTracks);
    CHECK(set.numScenes() == LiveSet::defaultScenes);
    std::set<std::int64_t> idsSeen;
    for (int t = 0; t < set.numTracks(); ++t)
    {
        CHECK(set.track(t).getChildWithName(ids::slots).getNumChildren() == set.numScenes());
        CHECK(idsSeen.insert(set.trackId(t)).second);
    }
    for (int s = 0; s < set.numScenes(); ++s)
        CHECK(idsSeen.insert(LiveSet::idOf(set.scene(s))).second);
    CHECK(static_cast<bool>(set.track(0)[ids::arm]));
    CHECK(set.tempo() == 120.0);
    CHECK(set.beatsPerBar() == 4.0);
}

TEST_CASE("Saving and reloading a set gives the same document", "[core][model]")
{
    juce::UndoManager undo;
    LiveSet set(&undo);
    fill(set);

    const juce::TemporaryFile temp(".lset");
    REQUIRE(set.saveToFile(temp.getFile()).wasOk());

    LiveSet loaded;
    const auto result = loaded.loadFromFile(temp.getFile());
    REQUIRE(result.wasOk());
    CHECK(loaded.state().toXmlString() == set.state().toXmlString());

    // Values come back as the same numbers.
    CHECK(loaded.tempo() == 97.5);
    CHECK(loaded.timeSigNumerator() == 7);
    CHECK(loaded.beatsPerBar() == 3.5);
    const auto clip = loaded.clip(0, 0);
    REQUIRE(clip.isValid());
    CHECK(static_cast<double>(clip[ids::loopEnd]) == 7.5);
    const auto notes = LiveSet::notesOf(clip);
    REQUIRE(notes.size() == 2);
    CHECK(notes[1].start == 1.25);
    CHECK(notes[1].length == 0.333333333333);
    CHECK(notes[1].velocity == 127);

    // Saved twice, byte for byte the same.
    const juce::TemporaryFile again(".lset");
    REQUIRE(loaded.saveToFile(again.getFile()).wasOk());
    CHECK(again.getFile().loadFileAsString() == temp.getFile().loadFileAsString());
}

TEST_CASE("Loading rejects files that are not valid sets", "[core][model]")
{
    LiveSet set;
    CHECK(set.loadFromXml(*juce::parseXML("<SOMETHING/>")).failed());
    CHECK(set.loadFromXml(
                 *juce::parseXML("<LIVESET formatVersion=\"99\"><TRACKS/><SCENES/><MASTER/></LIVESET>"))
              .failed());
    CHECK(
        set.loadFromXml(*juce::parseXML(
                            "<LIVESET formatVersion=\"1\"><TRACKS><TRACK name=\"x\"><SLOTS/></TRACK></TRACKS>"
                            "<SCENES><SCENE/></SCENES><MASTER/></LIVESET>"))
            .failed());
    CHECK(set.numTracks() == LiveSet::defaultTracks); // unchanged after a failed load
}

TEST_CASE("Every operation can be undone and redone", "[core][model]")
{
    using Op = std::function<void(LiveSet&)>;
    const std::vector<std::pair<const char*, Op>> operations{
        {"add track", [](LiveSet& s) { s.addTrack(1); }},
        {"delete track", [](LiveSet& s) { s.deleteTrack(0); }},
        {"duplicate track", [](LiveSet& s) { s.duplicateTrack(0); }},
        {"move track", [](LiveSet& s) { s.moveTrack(0, 2); }},
        {"add scene", [](LiveSet& s) { s.addScene(2); }},
        {"delete scene", [](LiveSet& s) { s.deleteScene(0); }},
        {"duplicate scene", [](LiveSet& s) { s.duplicateScene(0); }},
        {"move scene", [](LiveSet& s) { s.moveScene(0, 3); }},
        {"create clip", [](LiveSet& s) { s.createClip(1, 1, 4.0); }},
        {"delete clip", [](LiveSet& s) { s.deleteClip(0, 0); }},
        {"duplicate clip", [](LiveSet& s) { s.duplicateClip(0, 0); }},
        {"move clip", [](LiveSet& s) { s.moveClip(0, 0, 3, 7); }},
        {"clip property", [](LiveSet& s) { s.setClipProperty(s.clip(0, 0), ids::legato, true); }},
        {"clip loop", [](LiveSet& s) { s.setClipLoop(s.clip(0, 0), 0.5, 3.0); }},
        {"add note", [](LiveSet& s) { s.addNote(s.clip(0, 0), note(70, 2.0, 1.0)); }},
        {"remove note",
         [](LiveSet& s) {
             s.removeNote(s.clip(0, 0), s.clip(0, 0).getChildWithName(ids::notes).getChild(0));
         }},
        {"edit note",
         [](LiveSet& s) {
             s.setNote(s.clip(0, 0).getChildWithName(ids::notes).getChild(0), note(71, 3.0, 0.25));
         }},
        {"volume", [](LiveSet& s) { s.setTrackVolumeDb(0, -12.0); }},
        {"pan", [](LiveSet& s) { s.setTrackPan(0, -0.5); }},
        {"mute", [](LiveSet& s) { s.setTrackMute(0, true); }},
        {"solo", [](LiveSet& s) { s.setTrackSolo(1, true, false); }},
        {"arm", [](LiveSet& s) { s.setTrackArm(2, true, false); }},
        {"tuning", [](LiveSet& s) { s.setTrackTuning(1, "builtin.colundi"); }},
        {"master volume", [](LiveSet& s) { s.setMasterVolumeDb(-3.0); }},
        {"tempo", [](LiveSet& s) { s.setTempo(150.0); }},
        {"time signature", [](LiveSet& s) { s.setTimeSignature(5, 4); }},
        {"quantization", [](LiveSet& s) { s.setGlobalQuantization(Quantization::Sixteenth); }},
        {"metronome", [](LiveSet& s) { s.setMetronomeOn(false); }},
        {"scene tempo", [](LiveSet& s) { s.setSceneTempo(0, 88.0); }},
        {"scene time signature", [](LiveSet& s) { s.setSceneTimeSignature(0, 6, 8); }},
        {"stop button", [](LiveSet& s) { s.setHasStopButton(0, 1, false); }},
        {"track name", [](LiveSet& s) { s.setTrackName(0, "Drums"); }},
    };

    for (const auto& [name, op] : operations)
    {
        INFO(name);
        juce::UndoManager undo;
        LiveSet set(&undo);
        fill(set);
        undo.clearUndoHistory();

        const auto before = documentText(set);
        undo.beginNewTransaction(name);
        op(set);
        const auto after = documentText(set);
        CHECK(after != before);

        REQUIRE(undo.undo());
        CHECK(documentText(set) == before);
        REQUIRE(undo.redo());
        CHECK(documentText(set) == after);
    }
}

TEST_CASE("Solo and arm are exclusive unless added with a modifier", "[core][model]")
{
    LiveSet set;
    set.setTrackSolo(0, true, false);
    set.setTrackSolo(1, true, false);
    CHECK_FALSE(static_cast<bool>(set.track(0)[ids::solo]));
    CHECK(static_cast<bool>(set.track(1)[ids::solo]));
    set.setTrackSolo(2, true, true);
    CHECK(static_cast<bool>(set.track(1)[ids::solo]));
    CHECK(static_cast<bool>(set.track(2)[ids::solo]));

    set.setTrackArm(3, true, false);
    CHECK_FALSE(static_cast<bool>(set.track(0)[ids::arm]));
    CHECK(static_cast<bool>(set.track(3)[ids::arm]));
}

TEST_CASE("Scene operations keep one slot per scene in every track", "[core][model]")
{
    LiveSet set;
    set.createClip(1, 2, 4.0, "a");
    set.addScene(0);
    CHECK(set.clip(1, 3).isValid()); // moved down with its scene
    set.duplicateScene(3);
    CHECK(set.clip(1, 4).isValid());
    CHECK(LiveSet::idOf(set.clip(1, 4)) != LiveSet::idOf(set.clip(1, 3)));
    set.moveScene(4, 0);
    CHECK(set.clip(1, 0).isValid());
    set.deleteScene(0);
    for (int t = 0; t < set.numTracks(); ++t)
        CHECK(set.track(t).getChildWithName(ids::slots).getNumChildren() == set.numScenes());
    CHECK(set.numScenes() == LiveSet::defaultScenes + 1);
}

TEST_CASE("Duplicates get new IDs", "[core][model]")
{
    LiveSet set;
    set.createClip(0, 0, 4.0);
    const int copy = set.duplicateTrack(0);
    CHECK(set.trackId(copy) != set.trackId(0));
    CHECK(LiveSet::idOf(set.clip(copy, 0)) != LiveSet::idOf(set.clip(0, 0)));
    CHECK(set.duplicateClip(0, 0) == 1);
    CHECK(LiveSet::idOf(set.clip(0, 1)) != LiveSet::idOf(set.clip(0, 0)));
    CHECK(set.findClip(LiveSet::idOf(set.clip(0, 1))) == std::pair{0, 1});
}

TEST_CASE("Importing a MIDI file makes one clip per MIDI track", "[core][model][midi]")
{
    juce::MidiFile file;
    file.setTicksPerQuarterNote(960);
    juce::MidiMessageSequence first;
    first.addEvent(juce::MidiMessage::textMetaEvent(3, "Bass"), 0);
    first.addEvent(juce::MidiMessage::noteOn(1, 47, static_cast<juce::uint8>(100)), 0);
    first.addEvent(juce::MidiMessage::noteOff(1, 47), 480);
    first.addEvent(juce::MidiMessage::noteOn(2, 52, static_cast<juce::uint8>(64)), 960 * 4 + 240);
    first.addEvent(juce::MidiMessage::noteOff(2, 52), 960 * 5);
    juce::MidiMessageSequence second;
    second.addEvent(juce::MidiMessage::textMetaEvent(3, "Keys"), 0);
    second.addEvent(juce::MidiMessage::noteOn(1, 60, static_cast<juce::uint8>(90)), 1920);
    second.addEvent(juce::MidiMessage::noteOff(1, 60), 2880);
    juce::MidiMessageSequence conductor; // no notes: skipped
    conductor.addEvent(juce::MidiMessage::tempoMetaEvent(500000), 0);
    file.addTrack(conductor);
    file.addTrack(first);
    file.addTrack(second);

    juce::MemoryOutputStream out;
    REQUIRE(file.writeTo(out));
    juce::MemoryInputStream in(out.getData(), out.getDataSize(), false);

    juce::UndoManager undo;
    LiveSet set(&undo);
    undo.beginNewTransaction();
    const auto result = importMidi(set, in, "song", 1, 2);
    REQUIRE(result.ok());
    CHECK(result.clipsCreated == 2);

    const auto bass = set.clip(1, 2);
    REQUIRE(bass.isValid());
    CHECK(bass[ids::name].toString() == "song - Bass");
    CHECK(static_cast<double>(bass[ids::loopEnd]) == 8.0); // last note ends at beat 5: two bars
    const auto notes = LiveSet::notesOf(bass);
    REQUIRE(notes.size() == 2);
    CHECK(notes[0].pitch == 47);
    CHECK(notes[0].length == Catch::Approx(0.5));
    CHECK(notes[1].start == Catch::Approx(4.25));
    CHECK(notes[1].velocity == 64);

    const auto keys = set.clip(2, 2);
    REQUIRE(keys.isValid());
    CHECK(LiveSet::notesOf(keys).at(0).start == Catch::Approx(2.0));
    CHECK(static_cast<double>(keys[ids::loopEnd]) == 4.0);

    undo.undo();
    CHECK_FALSE(set.clip(1, 2).isValid());
    CHECK_FALSE(set.clip(2, 2).isValid());

    juce::MemoryInputStream garbage("not midi", 8, false);
    CHECK_FALSE(importMidi(set, garbage, "x", 0, 0).ok());
}

TEST_CASE("Importing a known .mid file produces the expected notes", "[core][model][midi]")
{
    // tests/data/midi/arp-and-bass.mid: format 1, 480 ticks per beat; a conductor track, "Arp"
    // (16 sixteenths over one bar) and "Bass" (four half notes on channel 2).
    const auto file = juce::File(LATTICE_SOURCE_DIR).getChildFile("tests/data/midi/arp-and-bass.mid");
    REQUIRE(file.existsAsFile());

    LiveSet set;
    const auto result = importMidiFile(set, file, 0, 0);
    REQUIRE(result.ok());
    CHECK(result.clipsCreated == 2);

    const auto arp = set.clip(0, 0);
    CHECK(arp[ids::name].toString() == "arp-and-bass - Arp");
    CHECK(static_cast<double>(arp[ids::loopEnd]) == 4.0);
    const auto arpNotes = LiveSet::notesOf(arp);
    REQUIRE(arpNotes.size() == 16);
    const int pattern[] = {40, 44, 47, 51, 54, 51, 47, 44};
    for (std::size_t i = 0; i < arpNotes.size(); ++i)
    {
        CHECK(arpNotes[i].pitch == pattern[i % 8]);
        CHECK(arpNotes[i].start == Catch::Approx(0.25 * static_cast<double>(i)));
        CHECK(arpNotes[i].length == Catch::Approx(0.125));
        CHECK(arpNotes[i].velocity == 90);
    }

    const auto bassNotes = LiveSet::notesOf(set.clip(1, 0));
    REQUIRE(bassNotes.size() == 4);
    CHECK(static_cast<double>(set.clip(1, 0)[ids::loopEnd]) == 8.0);
    CHECK(bassNotes[3].pitch == 35);
    CHECK(bassNotes[3].start == Catch::Approx(6.0));
    CHECK(bassNotes[3].length == Catch::Approx(2.0 - 1.0 / 480.0));
}
