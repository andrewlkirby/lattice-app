// Undo fuzzing (docs/05-m4-ui.md, Tests, item 4): 10,000 random operations through the UI's own
// paths (its commands, its controls' bindings, the piano roll's and the Session view's edits), then
// every one undone: the set must be exactly as it started. Then every one redone: exactly as it
// ended. What isn't undoable on purpose (how views show things, the next free ID) is left out.

#include "ui/shell/MainShell.h"
#include "ui/widgets/BoundControl.h"

#include <catch2/catch_test_macros.hpp>

using namespace lattice;
using namespace lattice::ui;

namespace
{
struct Fixture
{
    Fixture()
    {
        settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lattice-fuzz-" + juce::Uuid().toString() + ".settings");
        juce::PropertiesFile::Options options;
        options.applicationName = "LatticeTests";
        settings = std::make_unique<juce::PropertiesFile>(settingsFile, options);
        session.newSet(3, 3);
        session.flush();
        shell = std::make_unique<MainShell>(session, engine, *settings, tunings, ShellActions{});
        shell->setSize(1600, 1000);
        shell->setDetailVisible(true);
        // Questions are answered yes at once (deleting asks nothing, but a stray dialog must not wait).
        shell->deviceContext().confirm = [](const juce::String&, const juce::String&, const juce::String&, std::function<void()> yes) {
            if (yes)
                yes();
        };
    }
    ~Fixture()
    {
        shell.reset();
        settings.reset();
        settingsFile.deleteFile();
    }

    LiveSet& set() { return session.set(); }
    /** What the message loop does between events: the views catch up with the set. */
    void settle()
    {
        session.flush();
        shell->sessionView().flushUpdates();
        shell->deviceView().flushUpdates();
        shell->clipView().flushUpdates();
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings};
    juce::File settingsFile;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<MainShell> shell;
};

/** The set as XML, without what's not undoable on purpose. */
juce::String canonical(const juce::ValueTree& state)
{
    static const std::array<juce::Identifier, 5> notUndoable{ids::folded, ids::showAll, ids::padStart, ids::padPage, ids::nextId};
    std::function<void(juce::ValueTree&)> strip = [&](juce::ValueTree& node) {
        for (const auto& id : notUndoable)
            node.removeProperty(id, nullptr);
        for (auto child : node)
            strip(child);
    };
    auto copy = state.createCopy();
    strip(copy);
    return copy.toXmlString();
}

int numNotes(const LiveSet& set)
{
    int n = 0;
    for (int t = 0; t < set.numTracks(); ++t)
        for (int s = 0; s < set.numScenes(); ++s)
            if (const auto c = set.clip(t, s); c.isValid())
                n += c.getChildWithName(ids::notes).getNumChildren();
    return n;
}
} // namespace

TEST_CASE("Undo fuzzing: 10,000 UI operations, all undone back to the start and redone to the end", "[ui][undo][fuzz][slow]")
{
    Fixture f;
    auto& undo = f.session.undoManager();
    // Every step kept (the app keeps its usual limit; this checks the steps, not the limit).
    undo.setMaxNumberOfStoredUnits(std::numeric_limits<int>::max() / 4, 100'000);
    undo.clearUndoHistory();
    f.settle();
    const auto start = canonical(f.set().state());

    auto& commands = f.shell->commands();
    const juce::StringArray deviceTypes{"synth", "sampler", "eq8", "compressor", "reverb", "delay", "saturator", "utility", "instrumentRack"};
    const juce::StringArray editCommands{"create.midiClip", "create.midiTrack", "create.scene",   "create.group",  "create.captureScene",
                                         "edit.copy",       "edit.cut",         "edit.paste",     "edit.delete",   "edit.duplicate",
                                         "edit.ungroup",    "device.onOff",     "transport.tapTempo"};
    juce::Random random(20261101);
    std::map<juce::String, int> done; // what ran, for the report
    const auto pick = [&](const juce::StringArray& from) { return from[random.nextInt(from.size())]; };

    constexpr int operations = 10'000;
    for (int op = 0; op < operations; ++op)
    {
        auto& set = f.set();
        const int roll = random.nextInt(100);
        if (roll < 15)
        {
            // Select something: a slot, a track, a scene; sometimes a return's or the master's devices.
            const int kind = random.nextInt(10);
            if (kind == 9)
                f.shell->selection().showDevicesOf({random.nextBool() ? DeviceOwner::Kind::master : DeviceOwner::Kind::returnTrack, 0});
            else
                f.shell->selection().set({kind < 6 ? SessionSelection::Kind::slot : kind < 8 ? SessionSelection::Kind::track : SessionSelection::Kind::scene,
                                          random.nextInt(set.numTracks()), random.nextInt(set.numScenes())});
            ++done["select"];
        }
        else if (roll < 45)
        {
            // A command, as the menus, keys and palette run it (they're refused when not enabled).
            auto id = pick(editCommands);
            if ((id == "create.midiTrack" && set.numTracks() >= 10) || ((id == "create.scene" || id == "create.captureScene") && set.numScenes() >= 8))
                continue;
            if (random.nextInt(4) == 0)
                id = "device.insert." + pick(deviceTypes);
            if (commands.perform(id))
                ++done[id.upToLastOccurrenceOf(".", false, false) == "device.insert" ? juce::String("device.insert") : id];
        }
        else if (roll < 75)
        {
            // A control: set, dragged (one step however many writes) or reset.
            f.settle();
            std::vector<BoundControl*> controls;
            for (const auto* e : f.shell->controlRegistry().entries())
                if (auto* c = dynamic_cast<BoundControl*>(e->component.getComponent()))
                    controls.push_back(c);
            if (controls.empty())
                continue;
            auto& binding = controls[static_cast<std::size_t>(random.nextInt(static_cast<int>(controls.size())))]->binding();
            const int how = random.nextInt(3);
            if (how == 0)
                binding.setNormalised(random.nextFloat());
            else if (how == 1)
            {
                binding.beginGesture();
                for (int w = 1 + random.nextInt(3); --w >= 0;)
                    binding.setNormalised(random.nextFloat());
                binding.endGesture();
            }
            else
                binding.resetToDefault();
            ++done["control"];
        }
        else if (roll < 92)
        {
            // The piano roll, on a clip: one already there (selected as a click would), or a new one.
            std::vector<std::pair<int, int>> clips;
            for (int t = 0; t < set.numTracks(); ++t)
                for (int sc = 0; sc < set.numScenes(); ++sc)
                    if (set.clip(t, sc).isValid())
                        clips.push_back({t, sc});
            if (clips.empty() || random.nextInt(8) == 0)
            {
                f.shell->selection().set({SessionSelection::Kind::slot, random.nextInt(set.numTracks()), random.nextInt(set.numScenes())});
                commands.perform("create.midiClip");
            }
            else
            {
                const auto [t, sc] = clips[static_cast<std::size_t>(random.nextInt(static_cast<int>(clips.size())))];
                f.shell->selection().set({SessionSelection::Kind::slot, t, sc});
            }
            f.settle();
            auto& roll2 = f.shell->clipView().pianoRoll();
            if (!roll2.hasClip())
                continue;
            roll2.selectAll();
            if (numNotes(set) > 400)
            {
                roll2.edit("edit.delete");
                ++done["notes"];
                continue;
            }
            switch (random.nextInt(14))
            {
            case 0: roll2.doubleClick({roll2.getWidth() * (0.2f + 0.6f * random.nextFloat()), roll2.getHeight() * (0.2f + 0.6f * random.nextFloat())}); break;
            case 1: roll2.transposeSteps(random.nextBool() ? 1 : -1); break;
            case 2: roll2.nudge(random.nextBool() ? 1 : -1); break;
            case 3: roll2.resizeBy(random.nextBool() ? 1 : -1); break;
            case 4: roll2.quantize(); break;
            case 5: roll2.legato(); break;
            case 6: roll2.reverse(); break;
            case 7: roll2.invert(); break;
            case 8: roll2.stretch(random.nextBool() ? 2.0 : 0.5); break;
            case 9: roll2.split(); break;
            case 10: roll2.join(); break;
            case 11: roll2.rampVelocities(); break;
            case 12: random.nextBool() ? roll2.halveLoop() : roll2.doubleLoop(); break;
            default: roll2.toggleHighlightOfSelectedRows(); break;
            }
            ++done["notes"];
        }
        else
        {
            // The Session view's colouring, and a device inserted where the Device view looks.
            if (random.nextBool())
                f.shell->sessionView().colourSelection(random.nextInt(LiveSet::paletteSize()));
            else if (f.shell->deviceView().insertHere(drag::device("lattice." + pick(deviceTypes))))
                ++done["insert"];
            ++done["session"];
        }
        if (op % 50 == 0)
            f.settle();
    }
    f.settle();
    const auto end = canonical(f.set().state());
    const int steps = undo.getUndoDescriptions().size();
    juce::String report;
    for (const auto& [what, count] : done)
        report << what << " " << count << ", ";
    WARN("Undo fuzzing: " << steps << " undo steps; " << report);
    REQUIRE(steps > operations / 4);
    REQUIRE(end != start);

    int undone = 0;
    while (undo.undo())
        ++undone;
    CHECK(undone == steps);
    f.settle();
    CHECK(canonical(f.set().state()) == start);

    int redone = 0;
    while (undo.redo())
        ++redone;
    CHECK(redone == steps);
    f.settle();
    CHECK(canonical(f.set().state()) == end);
}
