#include "ui/widgets/ControlRegistry.h"

#include "ui/widgets/ValueBinding.h"

#include <algorithm>

namespace lattice::ui
{

ControlRegistry* ControlRegistry::findFor(const juce::Component& component)
{
    for (auto* c = component.getParentComponent(); c != nullptr; c = c->getParentComponent())
        if (auto* provider = dynamic_cast<Provider*>(c))
            return &provider->controlRegistry();
    return nullptr;
}

void ControlRegistry::place(juce::Component& control, const ValueBinding* binding, ControlRegistry*& current)
{
    auto* now = findFor(control);
    if (current != nullptr && current != now)
        current->remove(&control);
    current = now;
    if (now == nullptr)
        return;
    Entry entry{.component = &control,
                .binding = binding,
                .name = control.getTitle(),
                .group = binding != nullptr ? binding->spec().group : juce::String()};
    for (auto* c = control.getParentComponent(); c != nullptr; c = c->getParentComponent())
        if (const auto* locator = dynamic_cast<const Locator*>(c))
        {
            locator->describe(entry);
            break;
        }
    now->add(std::move(entry));
}

void ControlRegistry::add(Entry entry)
{
    const auto* key = entry.component.getComponent();
    if (key != nullptr)
        list.insert_or_assign(key, std::move(entry));
}

void ControlRegistry::remove(const juce::Component* component)
{
    list.erase(component);
}

std::vector<const ControlRegistry::Entry*> ControlRegistry::entries() const
{
    std::vector<const Entry*> out;
    out.reserve(list.size());
    for (const auto& [key, e] : list)
        if (e.component != nullptr)
            out.push_back(&e);
    return out;
}

const ControlRegistry::Entry* ControlRegistry::find(const juce::Component* component) const
{
    const auto it = list.find(component);
    return it != list.end() && it->second.component != nullptr ? &it->second : nullptr;
}

std::vector<const ControlRegistry::Entry*> ControlRegistry::search(const juce::String& text) const
{
    const auto words = juce::StringArray::fromTokens(text.toLowerCase(), true);
    std::vector<const Entry*> out;
    for (const auto* e : entries())
    {
        const auto haystack = (e->name + " " + e->group + " " + e->owner).toLowerCase();
        if (std::all_of(words.begin(), words.end(), [&](const juce::String& w) { return haystack.contains(w); }))
            out.push_back(e);
    }
    return out;
}

} // namespace lattice::ui
