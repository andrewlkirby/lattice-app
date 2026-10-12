#include "app/debug/ClipPropertiesPanel.h"

namespace lattice::debugui
{
namespace
{
constexpr int rowHeight = 26;
constexpr int labelWidth = 110;
} // namespace

ClipPropertiesPanel::ClipPropertiesPanel(SessionController& s, juce::ValueTree c)
    : session(s), clip(std::move(c))
{
    name.setText(clip[ids::name].toString(), juce::dontSendNotification);
    name.onReturnKey = [this] {
        edit(ids::name, name.getText());
    };
    name.onFocusLost = [this] {
        if (name.getText() != clip[ids::name].toString())
            edit(ids::name, name.getText());
    };
    addRow("Name", name);

    launchMode.addItemList({"Trigger", "Gate", "Toggle"}, 1);
    launchMode.setSelectedId(static_cast<int>(clip[ids::launchMode]) + 1, juce::dontSendNotification);
    launchMode.onChange = [this] {
        edit(ids::launchMode, launchMode.getSelectedId() - 1);
    };
    addRow("Launch mode", launchMode);

    for (int q = -1; q < numQuantizationValues; ++q)
        quantization.addItem(quantizationName(static_cast<Quantization>(q)), q + 2);
    quantization.setSelectedId(static_cast<int>(clip[ids::quantization]) + 2, juce::dontSendNotification);
    quantization.onChange = [this] {
        edit(ids::quantization, quantization.getSelectedId() - 2);
    };
    addRow("Quantization", quantization);

    legato.setToggleState(clip[ids::legato], juce::dontSendNotification);
    legato.onClick = [this] {
        edit(ids::legato, legato.getToggleState());
    };
    addRow({}, legato);

    looping.setToggleState(clip[ids::looping], juce::dontSendNotification);
    looping.onClick = [this] {
        edit(ids::looping, looping.getToggleState());
    };
    addRow({}, looping);

    addRow("Start marker", addBeatsSlider(ids::startMarker, 0.0));
    addRow("Loop start", addBeatsSlider(ids::loopStart, 0.0));
    addRow("Loop end", addBeatsSlider(ids::loopEnd, LiveSet::minLoopLength));

    for (auto* box : {&followA, &followB})
        for (int a = 0; a < numFollowActions; ++a)
            box->addItem(followActionName(static_cast<FollowAction>(a)), a + 1);
    followA.setSelectedId(static_cast<int>(clip[ids::followA]) + 1, juce::dontSendNotification);
    followB.setSelectedId(static_cast<int>(clip[ids::followB]) + 1, juce::dontSendNotification);
    followA.onChange = [this] {
        edit(ids::followA, followA.getSelectedId() - 1);
    };
    followB.onChange = [this] {
        edit(ids::followB, followB.getSelectedId() - 1);
    };
    addRow("Follow action A", followA);
    addRow("Follow action B", followB);

    chanceA.setRange(0.0, 100.0, 1.0);
    chanceA.setTextValueSuffix(" % A");
    chanceA.setSliderStyle(juce::Slider::LinearBar);
    chanceA.setValue(clip[ids::followChanceA], juce::dontSendNotification);
    chanceA.onDragStart = [this] {
        session.undoManager().beginNewTransaction("Follow chance");
    };
    chanceA.onValueChange = [this] {
        session.set().setClipProperty(clip, ids::followChanceA, chanceA.getValue());
    };
    addRow("Chance", chanceA);
    addRow("Follow time", addBeatsSlider(ids::followTime, 0.0));

    setSize(320, static_cast<int>(rows.size()) * rowHeight + 16);
}

void ClipPropertiesPanel::edit(const juce::Identifier& property, const juce::var& value)
{
    session.undoManager().beginNewTransaction("Edit clip");
    if (property == ids::loopStart || property == ids::loopEnd)
    {
        const double start = property == ids::loopStart ? static_cast<double>(value)
                                                        : static_cast<double>(clip[ids::loopStart]);
        const double end =
            property == ids::loopEnd ? static_cast<double>(value) : static_cast<double>(clip[ids::loopEnd]);
        session.set().setClipLoop(clip, start, end);
        return;
    }
    session.set().setClipProperty(clip, property, value);
}

juce::Slider& ClipPropertiesPanel::addBeatsSlider(const juce::Identifier& property, double minimum)
{
    auto slider = std::make_unique<juce::Slider>(juce::Slider::IncDecButtons, juce::Slider::TextBoxLeft);
    slider->setRange(minimum, 4096.0, 0.25);
    slider->setTextValueSuffix(" beats");
    slider->setValue(clip[property], juce::dontSendNotification);
    auto* raw = slider.get();
    slider->onValueChange = [this, raw, property] {
        edit(property, raw->getValue());
    };
    beatSliders.push_back(std::move(slider));
    return *raw;
}

void ClipPropertiesPanel::addRow(const juce::String& label, juce::Component& control)
{
    auto l = std::make_unique<juce::Label>(juce::String(), label);
    addAndMakeVisible(*l);
    addAndMakeVisible(control);
    rows.emplace_back(std::move(l), &control);
}

void ClipPropertiesPanel::paint(juce::Graphics& g)
{
    g.fillAll(colours::panel);
}

void ClipPropertiesPanel::resized()
{
    auto area = getLocalBounds().reduced(8);
    for (auto& [label, control] : rows)
    {
        auto row = area.removeFromTop(rowHeight).reduced(0, 2);
        label->setBounds(row.removeFromLeft(labelWidth));
        control->setBounds(row);
    }
}

} // namespace lattice::debugui
