#pragma once

#include "ui/commands/CommandRegistry.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lattice::ui
{

/** What the info view says about something (docs/05-m4-ui.md, 4a: "the hovered control's name, a
    short description, and its shortcut, as in Live"). */
struct InfoText
{
    juce::String title;
    juce::String body;
    juce::String shortcut;
    bool isEmpty() const { return title.isEmpty() && body.isEmpty(); }
};

/** The info text for a component: the first one, from it up through its parents, that has a title
    or runs a command (the "commandId" property). A bound control adds its value and how to use it;
    a command adds its description and shortcut from the registry. */
InfoText infoFor(const juce::Component* component, const CommandRegistry& commands);

/** The info view in the bottom-left corner. */
class InfoView final : public juce::Component
{
public:
    void show(const InfoText& text);
    const InfoText& shown() const noexcept { return current; }
    void paint(juce::Graphics&) override;

private:
    InfoText current;
};

} // namespace lattice::ui
