// The command system (docs/05-m4-ui.md, "One central command system"; Tests, item 3): commands run
// through the registry with one undo step each, shortcuts are saved by stable ID and have no
// conflicts by default, none takes a computer-keyboard letter, and the Session commands do what the
// debug UI's buttons and keys did, reaching the engine.

#include "control/ComputerKeyboardInput.h"
#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"
#include "ui/commands/SessionCommands.h"

#include <catch2/catch_test_macros.hpp>

#include <set>

using namespace lattice;
using namespace lattice::ui;
using namespace lattice::test;
using Kind = SessionSelection::Kind;

namespace
{
struct Fixture
{
    Fixture()
    {
        session.newSet(3, 4);
        session.flush();
        addSessionCommands(registry, session, {[this] { return selection; }, [this](SessionSelection s) { selection = s; }},
                           shell);
    }

    void render(std::int64_t samples)
    {
        session.flush();
        renderer.renderFor(samples);
        session.processEngineEvents();
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    OfflineRenderer renderer{engine, BlockPattern::Fixed64};
    SessionSelection selection;
    int renames = 0;
    ShellActions shell{{}, {}, {}, {}, {}, [this] { ++renames; }};
    CommandRegistry registry{&session.undoManager()};
};
} // namespace

TEST_CASE("Commands: the registry finds, runs and describes commands", "[ui][commands]")
{
    juce::UndoManager undo;
    juce::ValueTree tree("T");
    CommandRegistry registry(&undo);
    bool enabled = true;
    int runs = 0;
    const auto id = registry.add({.id = "test.bump",
                                  .name = "Bump",
                                  .category = "Test",
                                  .description = "Sets a property.",
                                  .defaultKeys = {juce::KeyPress('b', juce::ModifierKeys::commandModifier, 0)},
                                  .perform =
                                      [&] {
                                          ++runs;
                                          tree.setProperty("n", runs, &undo);
                                      },
                                  .isEnabled = [&] { return enabled; },
                                  .undoable = true});
    CHECK(registry.find("test.bump") == registry.find(id));
    CHECK(registry.numericIdOf("test.bump") == id);
    CHECK(registry.find("test.missing") == nullptr);
    CHECK(registry.shortcutsFor("test.bump").size() == 1);
    CHECK(registry.shortcutText("test.bump").isNotEmpty());

    CHECK(registry.perform("test.bump"));
    CHECK(registry.perform("test.bump"));
    CHECK(runs == 2);
    undo.undo(); // each run is its own undo step
    CHECK(static_cast<int>(tree["n"]) == 1);

    enabled = false;
    CHECK(!registry.perform("test.bump"));
    CHECK(runs == 2);
    CHECK(!registry.perform("test.missing"));

    // Through JUCE's command manager, as a menu or a key would run it.
    enabled = true;
    CHECK(registry.manager().invokeDirectly(id, false));
    CHECK(runs == 3);
    juce::ApplicationCommandInfo info(id);
    registry.getCommandInfo(id, info);
    CHECK(info.shortName == "Bump");
    CHECK(info.categoryName == "Test");
}

TEST_CASE("Commands: changed shortcuts are saved by stable ID and survive a reordering", "[ui][commands]")
{
    const auto add = [](CommandRegistry& r, const char* id, juce::KeyPress key) {
        r.add({.id = id, .name = id, .category = "Test", .defaultKeys = {key}, .perform = [] {}});
    };
    CommandRegistry a;
    add(a, "test.one", juce::KeyPress('1', juce::ModifierKeys::commandModifier, 0));
    add(a, "test.two", juce::KeyPress('2', juce::ModifierKeys::commandModifier, 0));
    add(a, "test.three", juce::KeyPress('3', juce::ModifierKeys::commandModifier, 0));
    auto& keys = a.keyMappings();
    keys.clearAllKeyPresses(a.numericIdOf("test.two"));
    keys.addKeyPress(a.numericIdOf("test.two"), juce::KeyPress(juce::KeyPress::F5Key));
    keys.clearAllKeyPresses(a.numericIdOf("test.three")); // removed
    const auto xml = a.shortcutsToXml();
    CHECK(xml->getNumChildElements() == 2); // only the changed ones

    // Another version, with the commands in another order and one more.
    CommandRegistry b;
    add(b, "test.new", juce::KeyPress('9', juce::ModifierKeys::commandModifier, 0));
    add(b, "test.three", juce::KeyPress('3', juce::ModifierKeys::commandModifier, 0));
    add(b, "test.two", juce::KeyPress('2', juce::ModifierKeys::commandModifier, 0));
    add(b, "test.one", juce::KeyPress('1', juce::ModifierKeys::commandModifier, 0));
    auto* unknown = xml->createNewChildElement("COMMAND"); // a command this version doesn't have
    unknown->setAttribute("id", "test.gone");
    b.restoreShortcuts(*xml);
    CHECK(b.shortcutsFor("test.two") == juce::Array<juce::KeyPress>{juce::KeyPress(juce::KeyPress::F5Key)});
    CHECK(b.shortcutsFor("test.three").isEmpty());
    CHECK(b.shortcutsFor("test.one").size() == 1);
    CHECK(b.shortcutsFor("test.new").size() == 1);

    b.resetShortcutsToDefaults();
    CHECK(b.shortcutsFor("test.three").size() == 1);
}

TEST_CASE("Commands: conflicts between shortcuts are found", "[ui][commands]")
{
    CommandRegistry r;
    const juce::KeyPress key('k', juce::ModifierKeys::commandModifier, 0);
    r.add({.id = "test.a", .name = "A", .category = "Test", .defaultKeys = {key}, .perform = [] {}});
    r.add({.id = "test.b", .name = "B", .category = "Test", .defaultKeys = {key}, .perform = [] {}});
    r.add({.id = "test.c", .name = "C", .category = "Test", .defaultKeys = {juce::KeyPress(juce::KeyPress::F2Key)}, .perform = [] {}});
    const auto found = r.conflicts();
    REQUIRE(found.size() == 1);
    CHECK(found[0].first == "test.a");
    CHECK(found[0].second == "test.b");
}

TEST_CASE("Commands: the Session commands are well formed, without conflicts or keyboard letters",
          "[ui][commands]")
{
    Fixture f;
    const auto all = f.registry.all();
    CHECK(all.size() >= 25);
    std::set<juce::String> ids;
    for (const auto* c : all)
    {
        INFO(c->id);
        CHECK(ids.insert(c->id).second);
        // "category.action": lower-case category, a dot, then a camelCase action.
        CHECK(c->id.matchesWildcard("*.*", false));
        CHECK(c->id.upToFirstOccurrenceOf(".", false, false).containsOnly("abcdefghijklmnopqrstuvwxyz"));
        CHECK(c->name.isNotEmpty());
        CHECK(c->category.isNotEmpty());
        CHECK(c->description.endsWithChar('.'));
        for (const auto& key : c->defaultKeys)
        {
            // The computer keyboard's letters, unmodified, belong to it (docs/02-m1-tuning-core.md).
            // (Compare whole key codes: a function key's code can end in a letter's.)
            bool keyboardLetter = false;
            for (const auto ch : juce::String(ComputerKeyboardInput::noteKeys) + "zxcvm")
                keyboardLetter = keyboardLetter || key.getKeyCode() == static_cast<int>(ch) ||
                                 key.getKeyCode() == static_cast<int>(juce::CharacterFunctions::toUpperCase(ch));
            CHECK(!(keyboardLetter && !key.getModifiers().isAnyModifierKeyDown()));
        }
    }
    CHECK(f.registry.conflicts().empty());
}

TEST_CASE("Commands: create, duplicate and delete act on the selection, one undo step each", "[ui][commands]")
{
    Fixture f;
    auto& set = f.session.set();
    REQUIRE(set.numTracks() == 3);

    f.selection = {Kind::track, 1, 0};
    CHECK(f.registry.perform("create.midiTrack"));
    CHECK(set.numTracks() == 4);
    CHECK(f.selection == SessionSelection{Kind::track, 2, 0}); // the new track
    f.session.undoManager().undo();
    CHECK(set.numTracks() == 3);

    // A clip: insert, duplicate below, delete.
    f.selection = {Kind::slot, 0, 1};
    CHECK(!f.registry.isEnabled("edit.duplicate")); // nothing to duplicate yet
    CHECK(f.registry.perform("create.midiClip"));
    CHECK(set.clip(0, 1).isValid());
    CHECK(!f.registry.isEnabled("create.midiClip")); // the slot is taken now
    CHECK(f.registry.perform("edit.duplicate"));
    CHECK(set.clip(0, 2).isValid());
    CHECK(f.selection == SessionSelection{Kind::slot, 0, 2});
    CHECK(f.registry.perform("edit.delete"));
    CHECK(!set.clip(0, 2).isValid());
    f.session.undoManager().undo();
    CHECK(set.clip(0, 2).isValid());

    // Scenes: insert after the selected one; a set keeps its last scene.
    f.selection = {Kind::scene, 0, 3};
    CHECK(f.registry.perform("create.scene"));
    CHECK(set.numScenes() == 5);
    CHECK(f.selection.scene == 4);
    for (int i = 0; i < 4; ++i)
        CHECK(f.registry.perform("edit.delete"));
    CHECK(set.numScenes() == 1);
    CHECK(!f.registry.isEnabled("edit.delete"));

    // Rename goes to the window's prompt.
    f.selection = {Kind::track, 0, 0};
    CHECK(f.registry.perform("edit.rename"));
    CHECK(f.renames == 1);
    CHECK(!f.registry.isEnabled("file.open")); // the window gave no file dialog
}

TEST_CASE("Commands: the arrow keys move the selection within the set", "[ui][commands]")
{
    Fixture f;
    f.selection = {Kind::slot, 0, 0};
    f.registry.perform("session.selectUp");
    f.registry.perform("session.selectLeft");
    CHECK(f.selection == SessionSelection{Kind::slot, 0, 0}); // stays inside
    f.registry.perform("session.selectRight");
    f.registry.perform("session.selectRight");
    f.registry.perform("session.selectRight");
    f.registry.perform("session.selectDown");
    CHECK(f.selection == SessionSelection{Kind::slot, 2, 1});
    f.selection = {Kind::track, 1, 0};
    f.registry.perform("session.selectDown"); // from a header into its slots
    CHECK(f.selection.kind == Kind::slot);
}

TEST_CASE("Commands: transport and launch commands reach the engine", "[ui][commands]")
{
    Fixture f;
    auto& set = f.session.set();
    set.createClip(0, 0, 4.0);
    f.render(4800);
    CHECK(!f.session.transport().playing);

    f.selection = {Kind::slot, 0, 0};
    CHECK(f.registry.perform("session.launch"));
    f.render(48000 * 3); // past the next bar at 120 BPM
    CHECK(f.session.transport().playing);
    CHECK(f.session.slotState(0, 0) == SlotState::Playing);
    CHECK(f.registry.find("transport.playStop")->isTicked());

    CHECK(f.registry.perform("transport.stopAllClips"));
    f.render(48000 * 3);
    CHECK(f.session.slotState(0, 0) == SlotState::Stopped);

    CHECK(f.registry.perform("transport.playStop"));
    f.render(4800);
    CHECK(!f.session.transport().playing);

    // A toggle reports its state for menus, and is one undo step.
    CHECK(!f.registry.find("transport.metronome")->isTicked());
    CHECK(f.registry.perform("transport.metronome"));
    CHECK(f.registry.find("transport.metronome")->isTicked());
    f.session.undoManager().undo();
    CHECK(!set.metronomeOn());
}
