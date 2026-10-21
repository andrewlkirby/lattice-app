// Group tracks in the model (docs/05-m4-ui.md, 4b): a group and its members are one contiguous
// block; tracks join the group they're added or moved into; deleting, duplicating, grouping and
// ungrouping keep the blocks whole, undo restores them, and they survive a save.

#include "core/model/LiveSet.h"

#include <catch2/catch_test_macros.hpp>

using namespace lattice;

namespace
{
juce::String names(const LiveSet& set)
{
    juce::StringArray out;
    for (int t = 0; t < set.numTracks(); ++t)
    {
        const auto indent = juce::String::repeatedString(">", set.groupDepth(t));
        out.add(indent + set.track(t)[ids::name].toString());
    }
    return out.joinIntoString(" ");
}

/** Four tracks A B C D; the caller groups them. */
struct Fixture
{
    Fixture()
    {
        set.resetToDefault(4, 2);
        for (int t = 0; t < 4; ++t)
            set.setTrackName(t, juce::String::charToString(static_cast<juce::juce_wchar>('A' + t)));
        undo.clearUndoHistory();
    }
    int group(int first, int last, const juce::String& name)
    {
        const int g = set.groupTracks(first, last);
        if (g >= 0)
            set.setTrackName(g, name);
        return g;
    }
    juce::UndoManager undo;
    LiveSet set{&undo};
};
} // namespace

TEST_CASE("Group tracks: grouping puts a group track before whole blocks at one level", "[core][model][groups]")
{
    Fixture f;
    CHECK(f.group(1, 2, "G") == 1);
    CHECK(names(f.set) == "A G >B >C D");
    CHECK(f.set.isGroupTrack(1));
    CHECK(f.set.parentGroupOf(2) == 1);
    CHECK(f.set.parentGroupOf(4) == -1);
    CHECK(f.set.blockEnd(1) == 4);
    CHECK(f.set.blockEnd(0) == 1);
    CHECK(f.set.groupMembers(1) == std::vector<int>{2, 3});

    // Nested: G and D into H.
    CHECK(f.group(1, 4, "H") == 1);
    CHECK(names(f.set) == "A H >G >>B >>C >D");
    CHECK(f.set.groupMembers(1) == std::vector<int>{2, 5}); // G's block counts as one member
    CHECK(f.set.groupLeaves(1) == std::vector<int>{3, 4, 5});
    CHECK(f.set.groupDepth(4) == 2);

    // Not whole blocks at one level: refused.
    CHECK(f.set.groupTracks(3, 5) == -1); // C and D are at different levels
    CHECK(f.set.groupTracks(0, 2) == -1); // cuts H in two
    CHECK(names(f.set) == "A H >G >>B >>C >D");

    // A group track has no clips of its own.
    CHECK(!f.set.createClip(1, 0, 4.0).isValid());
}

TEST_CASE("Group tracks: added and moved tracks join the group they land in", "[core][model][groups]")
{
    Fixture f;
    f.group(1, 2, "G"); // A G >B >C D
    f.set.addTrack(2);  // right after G's own track: inside
    f.set.setTrackName(2, "N");
    CHECK(names(f.set) == "A G >N >B >C D");
    f.set.addTrack(5); // before D: outside
    f.set.setTrackName(5, "O");
    CHECK(names(f.set) == "A G >N >B >C O D");

    // Out of the group, and back in.
    f.set.moveTrack(2, 6); // N after D (the end)
    CHECK(names(f.set) == "A G >B >C O D N");
    f.set.moveTrack(6, 3); // N up to where C is: before C, inside
    CHECK(names(f.set) == "A G >B >N >C O D");
    // Moving a group moves its members with it.
    f.set.moveTrack(1, 6); // G after D
    CHECK(names(f.set) == "A O D G >B >N >C");
    f.set.moveTrack(3, 0); // G to the top
    CHECK(names(f.set) == "G >B >N >C A O D");
    f.set.moveTrack(0, 2); // into itself: nothing
    CHECK(names(f.set) == "G >B >N >C A O D");

    f.undo.undo(); // every move, add and group is undoable
    f.undo.undo();
    f.undo.undo();
    f.undo.undo();
    f.undo.undo();
    f.undo.undo();
    f.undo.undo();
    CHECK(names(f.set) == "A B C D");
}

TEST_CASE("Group tracks: deleting or duplicating a group takes its members", "[core][model][groups]")
{
    Fixture f;
    f.group(1, 2, "G"); // A G >B >C D
    f.set.createClip(2, 0, 4.0);
    const auto gId = f.set.trackId(1);

    CHECK(f.set.duplicateTrack(1) == 4);
    CHECK(names(f.set) == "A G >B >C G copy >B >C D");
    CHECK(f.set.trackId(4) != gId);
    CHECK(f.set.parentGroupOf(5) == 4); // the copies are in the copied group
    CHECK(f.set.parentGroupOf(2) == 1);
    CHECK(f.set.clip(5, 0).isValid());
    CHECK(f.set.clip(5, 0)[ids::id] != f.set.clip(2, 0)[ids::id]);

    f.set.deleteTrack(4);
    CHECK(names(f.set) == "A G >B >C D");

    // Ungroup: the members stay, the group track goes (one undo step).
    f.undo.beginNewTransaction("Ungroup");
    f.set.ungroup(1);
    CHECK(names(f.set) == "A B C D");
    f.undo.undo();
    CHECK(names(f.set) == "A G >B >C D");

    // A set keeps a track that can hold clips.
    LiveSet lone;
    lone.resetToDefault(1, 1);
    lone.groupTracks(0, 0);
    lone.deleteTrack(0);
    CHECK(lone.numTracks() == 2);
}

TEST_CASE("Group tracks: a set with groups saves and loads the same", "[core][model][groups]")
{
    Fixture f;
    f.group(1, 2, "G");
    f.group(1, 4, "H");
    f.set.setGroupFolded(2, true);
    const auto xml = f.set.toXml();
    LiveSet loaded;
    REQUIRE(loaded.loadFromXml(*xml).wasOk());
    CHECK(names(loaded) == names(f.set));
    CHECK(loaded.groupLeaves(1) == f.set.groupLeaves(1));
    CHECK(static_cast<bool>(loaded.track(2)[ids::folded]));
}
