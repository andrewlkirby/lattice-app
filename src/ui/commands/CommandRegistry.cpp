#include "ui/commands/CommandRegistry.h"

namespace lattice::ui
{

CommandRegistry::CommandRegistry(juce::UndoManager* undoManager) : undo(undoManager)
{
    commands.setFirstCommandTarget(this);
}

CommandRegistry::~CommandRegistry()
{
    commands.setFirstCommandTarget(nullptr);
}

juce::CommandID CommandRegistry::add(Command command)
{
    jassert(command.id.isNotEmpty() && command.perform != nullptr);
    jassert(byId.find(command.id) == byId.end()); // IDs are unique
    const auto numeric = firstId + static_cast<juce::CommandID>(list.size());
    byId[command.id] = numeric;
    list.push_back(std::make_unique<Command>(std::move(command)));
    commands.registerCommand([&] {
        juce::ApplicationCommandInfo info(numeric);
        getCommandInfo(numeric, info);
        return info;
    }());
    return numeric;
}

const Command* CommandRegistry::find(const juce::String& id) const
{
    const auto it = byId.find(id);
    return it != byId.end() ? find(it->second) : nullptr;
}

const Command* CommandRegistry::find(juce::CommandID numericId) const
{
    const auto index = static_cast<std::size_t>(numericId - firstId);
    return numericId >= firstId && index < list.size() ? list[index].get() : nullptr;
}

juce::CommandID CommandRegistry::numericIdOf(const juce::String& id) const
{
    const auto it = byId.find(id);
    return it != byId.end() ? it->second : 0;
}

std::vector<const Command*> CommandRegistry::all() const
{
    std::vector<const Command*> out;
    out.reserve(list.size());
    for (const auto& c : list)
        out.push_back(c.get());
    return out;
}

bool CommandRegistry::isEnabled(const juce::String& id) const
{
    const auto* c = find(id);
    return c != nullptr && (!c->isEnabled || c->isEnabled());
}

bool CommandRegistry::perform(const juce::String& id)
{
    const auto* c = find(id);
    if (c == nullptr || !isEnabled(id))
        return false;
    if (c->undoable && undo != nullptr)
        undo->beginNewTransaction(c->name);
    c->perform();
    commands.commandStatusChanged(); // enabled and ticked states may have changed
    return true;
}

juce::Array<juce::KeyPress> CommandRegistry::shortcutsFor(const juce::String& id) const
{
    const auto numeric = numericIdOf(id);
    return numeric != 0 ? commands.getKeyMappings()->getKeyPressesAssignedToCommand(numeric) : juce::Array<juce::KeyPress>();
}

juce::String CommandRegistry::shortcutText(const juce::String& id) const
{
    juce::StringArray keys;
    for (const auto& k : shortcutsFor(id))
        keys.add(k.getTextDescriptionWithIcons());
    return keys.joinIntoString(", ");
}

std::vector<CommandRegistry::Conflict> CommandRegistry::conflicts() const
{
    std::vector<Conflict> out;
    std::vector<std::pair<juce::KeyPress, juce::String>> seen;
    for (const auto& c : list)
        for (const auto& key : shortcutsFor(c->id))
        {
            for (const auto& [otherKey, otherId] : seen)
                if (otherKey == key)
                    out.push_back({key, otherId, c->id});
            seen.emplace_back(key, c->id);
        }
    return out;
}

std::unique_ptr<juce::XmlElement> CommandRegistry::shortcutsToXml() const
{
    auto xml = std::make_unique<juce::XmlElement>("SHORTCUTS");
    for (const auto& c : list)
    {
        const auto keys = shortcutsFor(c->id);
        if (keys == c->defaultKeys)
            continue;
        auto* command = xml->createNewChildElement("COMMAND");
        command->setAttribute("id", c->id);
        for (const auto& k : keys)
            command->createNewChildElement("KEY")->setAttribute("key", k.getTextDescription());
    }
    return xml;
}

void CommandRegistry::restoreShortcuts(const juce::XmlElement& xml)
{
    auto& mappings = *commands.getKeyMappings();
    for (const auto* command : xml.getChildWithTagNameIterator("COMMAND"))
    {
        const auto numeric = numericIdOf(command->getStringAttribute("id"));
        if (numeric == 0)
            continue; // a command this version doesn't have
        mappings.clearAllKeyPresses(numeric);
        for (const auto* key : command->getChildWithTagNameIterator("KEY"))
        {
            const auto k = juce::KeyPress::createFromDescription(key->getStringAttribute("key"));
            if (k.isValid())
                mappings.addKeyPress(numeric, k);
        }
    }
}

void CommandRegistry::resetShortcutsToDefaults()
{
    commands.getKeyMappings()->resetToDefaultMappings();
}

void CommandRegistry::getAllCommands(juce::Array<juce::CommandID>& ids)
{
    for (std::size_t i = 0; i < list.size(); ++i)
        ids.add(firstId + static_cast<juce::CommandID>(i));
}

void CommandRegistry::getCommandInfo(juce::CommandID numericId, juce::ApplicationCommandInfo& info)
{
    const auto* c = find(numericId);
    if (c == nullptr)
        return;
    info.setInfo(c->name, c->description, c->category, 0);
    info.setActive(!c->isEnabled || c->isEnabled());
    if (c->onKeyUp)
        info.flags |= juce::ApplicationCommandInfo::wantsKeyUpDownCallbacks;
    if (c->isTicked)
        info.setTicked(c->isTicked());
    info.defaultKeypresses = c->defaultKeys;
}

bool CommandRegistry::perform(const InvocationInfo& info)
{
    const auto* c = find(info.commandID);
    if (c == nullptr)
        return false;
    if (info.invocationMethod == InvocationInfo::fromKeyPress && !info.isKeyDown)
    {
        if (c->onKeyUp)
            c->onKeyUp();
        return true;
    }
    return perform(c->id);
}

} // namespace lattice::ui
