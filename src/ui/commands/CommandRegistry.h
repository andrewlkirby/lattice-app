#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <map>
#include <memory>
#include <vector>

namespace lattice::ui
{

/** One user action (docs/05-m4-ui.md, "One central command system"). Menus, shortcuts, context
    menus, Find, Key Map mode, and later Virtual Push and the controllers all run the same commands,
    which is how every feature stays reachable with no hardware. */
struct Command
{
    /** Stable forever once released, like parameter IDs: saved shortcuts and mappings refer to it.
        "category.action", e.g. "transport.playStop". */
    juce::String id = {};
    juce::String name = {};        // "Play/Stop"
    juce::String category = {};    // "Transport"
    juce::String description = {}; // one sentence for the info view and Find
    juce::Array<juce::KeyPress> defaultKeys = {};
    std::function<void()> perform = {};
    std::function<bool()> isEnabled = {}; // empty: always enabled
    std::function<bool()> isTicked = {};  // for on/off commands (Metronome); empty: not a toggle
    /** Starts an undo transaction named after the command before performing it, so a command that
        edits the set is one undo step. */
    bool undoable = false;
    /** For a command whose key is held (launching a Gate clip with Enter): called when its
        shortcut key is released. Empty: key releases are ignored. */
    std::function<void()> onKeyUp = {};
};

/** All commands, on top of JUCE's ApplicationCommandManager (so menus and key mappings work as
    JUCE's do), keyed by their stable IDs. Shortcuts are saved by stable ID, not by JUCE's numeric
    IDs, so they survive commands being added or reordered. Message thread only. */
class CommandRegistry final : public juce::ApplicationCommandTarget
{
public:
    explicit CommandRegistry(juce::UndoManager* undo = nullptr);
    ~CommandRegistry() override;

    /** Adds a command; its default keys become its shortcuts. Returns its JUCE command ID. */
    juce::CommandID add(Command command);

    const Command* find(const juce::String& id) const;
    const Command* find(juce::CommandID numericId) const;
    juce::CommandID numericIdOf(const juce::String& id) const; // 0 if there's none
    /** Every command, in the order they were added. */
    std::vector<const Command*> all() const;

    bool isEnabled(const juce::String& id) const;
    /** Runs a command if it exists and is enabled. Returns whether it ran. */
    bool perform(const juce::String& id);

    juce::ApplicationCommandManager& manager() noexcept { return commands; }
    juce::KeyPressMappingSet& keyMappings() noexcept { return *commands.getKeyMappings(); }
    juce::Array<juce::KeyPress> shortcutsFor(const juce::String& id) const;
    /** A command's shortcuts as text ("Ctrl+D"), for menus, the info view and Find. */
    juce::String shortcutText(const juce::String& id) const;

    /** Two commands that share a key. */
    struct Conflict
    {
        juce::KeyPress key;
        juce::String first, second; // command IDs
    };
    std::vector<Conflict> conflicts() const;

    /** The shortcuts that differ from the defaults, by stable ID:
        <SHORTCUTS><COMMAND id="edit.duplicate"><KEY key="ctrl + D"/></COMMAND>...</SHORTCUTS>
        A command listed with no keys has had its shortcuts removed. */
    std::unique_ptr<juce::XmlElement> shortcutsToXml() const;
    /** Applies saved shortcuts over the defaults. Unknown command IDs (from a newer version, or a
        removed command) are skipped. */
    void restoreShortcuts(const juce::XmlElement& xml);
    void resetShortcutsToDefaults();

    // juce::ApplicationCommandTarget
    juce::ApplicationCommandTarget* getNextCommandTarget() override { return nullptr; }
    void getAllCommands(juce::Array<juce::CommandID>& ids) override;
    void getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info) override;
    bool perform(const InvocationInfo& info) override;

private:
    static constexpr juce::CommandID firstId = 0x4c000; // clear of JUCE's own StandardApplicationCommandIDs

    juce::UndoManager* undo;
    juce::ApplicationCommandManager commands;
    std::vector<std::unique_ptr<Command>> list;
    std::map<juce::String, juce::CommandID> byId;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(CommandRegistry)
};

} // namespace lattice::ui
