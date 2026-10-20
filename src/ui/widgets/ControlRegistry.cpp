#include "ui/widgets/ControlRegistry.h"

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

void ControlRegistry::add(Entry entry)
{
    remove(entry.component.getComponent());
    list.push_back(std::move(entry));
}

void ControlRegistry::remove(const juce::Component* component)
{
    std::erase_if(list, [component](const Entry& e) { return e.component == nullptr || e.component.getComponent() == component; });
}

std::vector<const ControlRegistry::Entry*> ControlRegistry::entries() const
{
    std::vector<const Entry*> out;
    for (const auto& e : list)
        if (e.component != nullptr)
            out.push_back(&e);
    return out;
}

const ControlRegistry::Entry* ControlRegistry::find(const juce::Component* component) const
{
    for (const auto& e : list)
        if (e.component != nullptr && e.component.getComponent() == component)
            return &e;
    return nullptr;
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
