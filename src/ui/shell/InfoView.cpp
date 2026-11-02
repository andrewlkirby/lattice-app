#include "ui/shell/InfoView.h"

#include "ui/theme/LatticeLookAndFeel.h"
#include "ui/widgets/BoundControl.h"
#include "ui/widgets/NumberBox.h"
#include "ui/widgets/Toggle.h"

namespace lattice::ui
{

InfoText infoFor(const juce::Component* component, const CommandRegistry& commands)
{
    for (auto* c = component; c != nullptr; c = c->getParentComponent())
    {
        if (const auto* control = dynamic_cast<const BoundControl*>(c))
        {
            const auto& spec = control->binding().spec();
            const bool box = dynamic_cast<const NumberBox*>(c) != nullptr;
            InfoText info;
            info.title = c->getTitle();
            info.body = (spec.group.isNotEmpty() ? spec.group + juce::String::fromUTF8(" \xc2\xb7 ") : juce::String()) +
                        control->binding().text() + "\n" +
                        (spec.kind == ParamKind::Choice && box
                             ? "Click for the list, or drag. Delete: default."
                             : juce::String(box ? "Drag (Shift: fine). Double-click to type. Delete: default."
                                                : "Drag (Shift: fine). Double-click: default. Enter to type."));
            return info;
        }

        const auto commandId = c->getProperties()["commandId"].toString();
        if (commandId.isNotEmpty())
            if (const auto* command = commands.find(commandId))
                return {command->name, command->description, commands.shortcutText(commandId)};

        if (c->getTitle().isNotEmpty())
        {
            InfoText info{c->getTitle(), c->getHelpText(), {}};
            if (const auto* toggle = dynamic_cast<const Toggle*>(c); toggle != nullptr && info.body.isEmpty())
                info.body = toggle->isOn() ? "On" : "Off";
            return info;
        }
    }
    return {};
}

void InfoView::show(const InfoText& text)
{
    if (text.title == current.title && text.body == current.body && text.shortcut == current.shortcut)
        return;
    current = text;
    repaint();
}

void InfoView::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    g.fillAll(t.surface);
    g.setColour(t.border);
    g.fillRect(getLocalBounds().removeFromTop(1));
    auto area = getLocalBounds().reduced(juce::roundToInt(t.padding));
    if (current.isEmpty())
        return;

    g.setColour(t.text);
    g.setFont(juce::Font(t.font(t.fontNormal)).boldened());
    g.drawFittedText(current.title, area.removeFromTop(juce::roundToInt(t.fontNormal + 4.0f)), juce::Justification::topLeft, 1);
    if (current.shortcut.isNotEmpty())
    {
        g.setColour(t.accent);
        g.setFont(t.font(t.fontSmall));
        g.drawFittedText(current.shortcut, area.removeFromBottom(juce::roundToInt(t.fontSmall + 4.0f)),
                         juce::Justification::bottomLeft, 1);
    }
    g.setColour(t.textDim);
    g.setFont(t.font(t.fontSmall));
    g.drawFittedText(current.body, area, juce::Justification::topLeft, juce::jmax(1, area.getHeight() / juce::roundToInt(t.fontSmall + 3.0f)));
}

} // namespace lattice::ui
