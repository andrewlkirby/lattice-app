#include "ui/device/DeviceBody.h"

#include "ui/device/DevicePanels.h"
#include "ui/device/DeviceView.h"
#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui
{

// --- DeviceBody --------------------------------------------------------------------------------------

DeviceBody::DeviceBody(DeviceContext& c, juce::ValueTree device) : context(c), node(std::move(device))
{
}

const StudioDevice& DeviceBody::prototype() const
{
    return *context.specs.forType(node[ids::deviceType].toString());
}

bool DeviceBody::reveal(const juce::String& parameterId)
{
    return controlFor(parameterId) != nullptr;
}

juce::Component* DeviceBody::controlFor(const juce::String& parameterId)
{
    for (auto* c : boundControlsIn(*this))
        if (parameterIdOf(*c) == parameterId)
        {
            // Not one of a nested device's (a rack's chain shows its devices' own frames).
            for (auto* p = c->getParentComponent(); p != nullptr && p != this; p = p->getParentComponent())
                if (dynamic_cast<DeviceBody*>(p) != nullptr)
                    return nullptr;
            return c;
        }
    return nullptr;
}

void DeviceBody::widthChanged()
{
    if (context.view != nullptr)
        context.view->requestLayout();
}

// --- GenericBody ---------------------------------------------------------------------------------------

GenericBody::GenericBody(DeviceContext& c, juce::ValueTree device) : DeviceBody(c, std::move(device))
{
    const auto& proto = prototype();
    for (const auto& group : proto.parameterGroups())
    {
        auto section = std::make_unique<ParameterSection>(group.name);
        for (const int index : group.parameters)
            section->add(makeParameterControl(context.set(), node, proto.parameter(index).spec()));
        addAndMakeVisible(*section);
        groups.push_back(std::move(section));
    }
}

int GenericBody::preferredWidth(int height) const
{
    const auto& t = themeOf(*this);
    int w = 0;
    for (const auto& s : groups)
        w += s->preferredWidth(height) + juce::roundToInt(t.spacing);
    return juce::jmax(juce::roundToInt(t.controlHeight * 4.0f), w);
}

void GenericBody::resized()
{
    const auto& t = themeOf(*this);
    auto r = getLocalBounds();
    for (auto& s : groups)
    {
        s->setBounds(r.removeFromLeft(s->preferredWidth(r.getHeight())));
        r.removeFromLeft(juce::roundToInt(t.spacing));
    }
}

// --- PanelRow ----------------------------------------------------------------------------------------

int PanelRow::gap() const
{
    return juce::roundToInt(themeOf(*this).padding);
}

int PanelRow::displayWidth(int height, float aspect) const
{
    const auto& t = themeOf(*this);
    return juce::jlimit(juce::roundToInt(t.controlHeight * 4.0f), juce::roundToInt(t.controlHeight * 16.0f),
                        juce::roundToInt(static_cast<float>(height) * aspect));
}

ParameterSection* PanelRow::addSection(const juce::String& title, const juce::StringArray& parameterIds, ControlStyle style)
{
    auto section = std::make_unique<ParameterSection>(title);
    section->addParameters(context.set(), node, prototype(), parameterIds, style);
    auto* raw = section.get();
    addPart(std::move(section), [raw](int height) { return raw->preferredWidth(height); });
    return raw;
}

int PanelRow::preferredWidth(int height) const
{
    int w = 0;
    for (const auto& p : parts)
        if (p.component->isVisible())
            w += p.widthFor(height) + gap();
    return juce::jmax(0, w - gap());
}

void PanelRow::resized()
{
    auto r = getLocalBounds();
    for (auto& p : parts)
    {
        if (!p.component->isVisible())
            continue;
        p.component->setBounds(r.removeFromLeft(p.widthFor(r.getHeight())));
        r.removeFromLeft(gap());
    }
}

// --- Which body ----------------------------------------------------------------------------------------

bool hasOwnPanel(const juce::String& typeId)
{
    return panels::factoryFor(typeId) != nullptr;
}

std::unique_ptr<DeviceBody> createDeviceBody(DeviceContext& context, const juce::ValueTree& device, bool allParameters)
{
    const auto type = device[ids::deviceType].toString();
    if (!allParameters)
        if (const auto factory = panels::factoryFor(type))
            return factory(context, device);
    return std::make_unique<GenericBody>(context, device);
}

} // namespace lattice::ui
