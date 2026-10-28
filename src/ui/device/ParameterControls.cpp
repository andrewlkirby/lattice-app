#include "ui/device/ParameterControls.h"

#include "devices/framework/StudioDevice.h"
#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui
{

// --- LabelledBox -------------------------------------------------------------------------------------

LabelledBox::LabelledBox(std::unique_ptr<ValueBinding> binding) : numberBox(std::move(binding))
{
    label = numberBox.binding().spec().name;
    setTitle(label);
    addAndMakeVisible(numberBox);
}

void LabelledBox::setLabel(const juce::String& text)
{
    label = text;
    numberBox.setTitle(text);
    repaint();
}

juce::Rectangle<int> LabelledBox::preferredSize() const
{
    const auto& t = themeOf(*this);
    const auto box = numberBox.preferredSize();
    const float labelWidth = juce::GlyphArrangement::getStringWidth(juce::Font(t.font(t.fontSmall)), label);
    return {0, 0, juce::jmax(box.getWidth(), juce::roundToInt(labelWidth + t.spacing)),
            box.getHeight() + juce::roundToInt(t.fontSmall + 4.0f)};
}

void LabelledBox::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    g.setFont(t.font(t.fontSmall));
    g.setColour(isEnabled() ? t.textDim : t.textDisabled);
    g.drawFittedText(label, getLocalBounds().removeFromTop(juce::roundToInt(t.fontSmall + 4.0f)), juce::Justification::centred, 1,
                     0.8f);
}

void LabelledBox::resized()
{
    const auto& t = themeOf(*this);
    auto r = getLocalBounds();
    r.removeFromTop(juce::roundToInt(t.fontSmall + 4.0f));
    numberBox.setBounds(r.withSizeKeepingCentre(r.getWidth(), juce::jmin(r.getHeight(), numberBox.preferredSize().getHeight())));
}

// --- Making and measuring controls ------------------------------------------------------------------------

std::unique_ptr<juce::Component> makeParameterControl(LiveSet& set, const juce::ValueTree& device, const ParameterSpec& spec,
                                                      ControlStyle style)
{
    if (style == ControlStyle::automatic)
        style = spec.kind == ParamKind::Bool    ? ControlStyle::toggle
                : spec.kind == ParamKind::Float ? ControlStyle::knob
                                                : ControlStyle::box;
    auto binding = ValueBinding::deviceParameter(set, device, spec);
    switch (style)
    {
    case ControlStyle::toggle:
        return std::make_unique<Toggle>(std::move(binding), Toggle::Role::accent, spec.name);
    case ControlStyle::box:
        return std::make_unique<LabelledBox>(std::move(binding));
    case ControlStyle::knob:
    case ControlStyle::automatic:
        break;
    }
    return std::make_unique<Knob>(std::move(binding));
}

juce::Rectangle<int> preferredSizeOf(const juce::Component& control)
{
    if (const auto* knob = dynamic_cast<const Knob*>(&control))
        return knob->preferredSize();
    if (const auto* labelled = dynamic_cast<const LabelledBox*>(&control))
        return labelled->preferredSize();
    if (const auto* box = dynamic_cast<const NumberBox*>(&control))
        return box->preferredSize();
    if (const auto* toggle = dynamic_cast<const Toggle*>(&control))
        return toggle->preferredSize();
    return control.getLocalBounds();
}

ValueBinding* bindingOf(juce::Component& control)
{
    if (auto* bound = dynamic_cast<BoundControl*>(&control))
        return &bound->binding();
    if (auto* labelled = dynamic_cast<LabelledBox*>(&control))
        return &labelled->box().binding();
    if (auto* toggle = dynamic_cast<Toggle*>(&control))
        return toggle->binding();
    return nullptr;
}

juce::String parameterIdOf(juce::Component& control)
{
    const auto* b = bindingOf(control);
    return b != nullptr ? b->spec().id : juce::String();
}

std::vector<juce::Component*> boundControlsIn(juce::Component& root)
{
    std::vector<juce::Component*> out;
    std::function<void(juce::Component&)> visit = [&](juce::Component& c) {
        if (dynamic_cast<LabelledBox*>(&c) != nullptr || dynamic_cast<BoundControl*>(&c) != nullptr ||
            (dynamic_cast<Toggle*>(&c) != nullptr && bindingOf(c) != nullptr))
        {
            out.push_back(&c);
            return; // a labelled box's own number box isn't a second control
        }
        for (auto* child : c.getChildren())
            visit(*child);
    };
    visit(root);
    return out;
}

int flowColumns(const std::vector<juce::Component*>& items, juce::Rectangle<int> area, int gap, bool place)
{
    int x = 0, y = 0, columnWidth = 0;
    std::vector<juce::Component*> column;
    const auto flush = [&] {
        if (place)
            for (auto* c : column)
                c->setBounds(c->getX(), c->getY(), columnWidth, c->getHeight());
        x += columnWidth + gap;
        column.clear();
        columnWidth = 0;
        y = 0;
    };
    for (auto* item : items)
    {
        if (!item->isVisible())
            continue;
        const auto size = preferredSizeOf(*item);
        if (y > 0 && y + size.getHeight() > area.getHeight())
            flush();
        if (place)
            item->setBounds(area.getX() + x, area.getY() + y, size.getWidth(), size.getHeight());
        column.push_back(item);
        columnWidth = juce::jmax(columnWidth, size.getWidth());
        y += size.getHeight() + gap;
    }
    if (!column.empty())
        flush();
    return juce::jmax(0, x - gap);
}

// --- ParameterSection ------------------------------------------------------------------------------------

ParameterSection::ParameterSection(juce::String title) : heading(std::move(title))
{
    setTitle(heading);
}

juce::Component* ParameterSection::add(std::unique_ptr<juce::Component> control)
{
    auto* raw = control.get();
    addAndMakeVisible(*raw);
    items.push_back(raw);
    owned.push_back(std::move(control));
    return raw;
}

void ParameterSection::addParameters(LiveSet& set, const juce::ValueTree& device, const StudioDevice& prototype,
                                     const juce::StringArray& parameterIds, ControlStyle style)
{
    for (const auto& id : parameterIds)
        if (const auto* p = prototype.findParameter(id))
            add(makeParameterControl(set, device, p->spec(), style));
}

int ParameterSection::titleHeight() const
{
    const auto& t = themeOf(*this);
    return heading.isEmpty() ? 0 : juce::roundToInt(t.fontSmall + t.spacing);
}

juce::Rectangle<int> ParameterSection::controlArea(juce::Rectangle<int> bounds) const
{
    const auto& t = themeOf(*this);
    bounds.removeFromTop(titleHeight());
    return bounds.reduced(juce::roundToInt(t.padding * 0.5f), 0);
}

int ParameterSection::preferredWidth(int height) const
{
    const auto& t = themeOf(*this);
    const auto area = controlArea({0, 0, 10000, height});
    const int columns = flowColumns(items, area, juce::roundToInt(t.spacing), false);
    const int titleWidth = heading.isEmpty() ? 0
                                             : juce::roundToInt(juce::GlyphArrangement::getStringWidth(
                                                                    juce::Font(t.font(t.fontSmall)).boldened(), heading.toUpperCase()) +
                                                                t.padding);
    return juce::jmax(columns, titleWidth) + juce::roundToInt(t.padding);
}

void ParameterSection::paint(juce::Graphics& g)
{
    if (heading.isEmpty())
        return;
    const auto& t = themeOf(*this);
    g.setFont(juce::Font(t.font(t.fontSmall)).boldened());
    g.setColour(t.textDim);
    g.drawText(heading.toUpperCase(), getLocalBounds().removeFromTop(titleHeight()).reduced(juce::roundToInt(t.padding * 0.5f), 0),
               juce::Justification::centredLeft, true);
}

void ParameterSection::resized()
{
    const auto& t = themeOf(*this);
    flowColumns(items, controlArea(getLocalBounds()), juce::roundToInt(t.spacing), true);
}

} // namespace lattice::ui
