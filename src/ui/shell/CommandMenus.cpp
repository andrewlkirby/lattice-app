#include "ui/shell/CommandMenus.h"

namespace lattice::ui
{
namespace
{
const juce::StringArray categoryOrder{"File", "Edit", "Create", "Session", "Clip", "Device", "Tuning", "Transport", "View"};
}

CommandMenus::CommandMenus(CommandRegistry& registry) : commands(registry)
{
    setApplicationCommandManagerToWatch(&commands.manager());
    commands.keyMappings().addChangeListener(this); // shortcuts shown in the menus follow changes
}

CommandMenus::~CommandMenus()
{
    commands.keyMappings().removeChangeListener(this);
    setApplicationCommandManagerToWatch(nullptr);
}

juce::StringArray CommandMenus::getMenuBarNames()
{
    juce::StringArray names;
    for (const auto& category : categoryOrder)
        for (const auto* c : commands.all())
            if (c->category == category)
            {
                names.add(category);
                break;
            }
    for (const auto* c : commands.all())
        if (!categoryOrder.contains(c->category))
            names.addIfNotAlreadyThere(c->category);
    return names;
}

juce::PopupMenu CommandMenus::getMenuForIndex(int, const juce::String& menuName)
{
    juce::PopupMenu menu;
    for (const auto* c : commands.all())
        if (c->category == menuName)
        {
            menu.addCommandItem(&commands.manager(), commands.numericIdOf(c->id));
            for (const auto& s : submenus)
                if (s.after == c->id && s.make)
                    menu.addSubMenu(s.name, s.make());
        }
    return menu;
}

void CommandMenus::addSubmenuAfter(const juce::String& commandId, const juce::String& name, std::function<juce::PopupMenu()> make)
{
    submenus.push_back({commandId, name, std::move(make)});
    menuItemsChanged();
}

} // namespace lattice::ui
