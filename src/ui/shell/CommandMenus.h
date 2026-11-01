#pragma once

#include "ui/commands/CommandRegistry.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lattice::ui
{

/** The window's menu bar, built from the command registry: one menu per category, in a fixed
    order (File, Edit, Create, Session, Transport, View, then any others), with each command's
    shortcut and tick shown by JUCE. Every command is reachable from here, so nothing depends on
    remembering a shortcut. Message thread only. */
class CommandMenus final : public juce::MenuBarModel, private juce::ChangeListener
{
public:
    explicit CommandMenus(CommandRegistry& registry);
    ~CommandMenus() override;

    juce::StringArray getMenuBarNames() override;
    juce::PopupMenu getMenuForIndex(int topLevelMenuIndex, const juce::String& menuName) override;
    void menuItemSelected(int, int) override {}

    /** A submenu after a command's item (File > Open Recent after Open): (command ID, name, the
        submenu, made when the menu opens). */
    void addSubmenuAfter(const juce::String& commandId, const juce::String& name, std::function<juce::PopupMenu()> make);

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { menuItemsChanged(); }

    CommandRegistry& commands;
    struct Submenu
    {
        juce::String after, name;
        std::function<juce::PopupMenu()> make;
    };
    std::vector<Submenu> submenus;
};

} // namespace lattice::ui
