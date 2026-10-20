#include "app/debug/DeviceUi.h"

namespace lattice::debugui
{

ParameterControl::ParameterControl(SessionController& s, juce::ValueTree d, const DeviceParameter& prototype)
    : session(s), device(std::move(d)), parameterSpec(prototype.spec()), range(prototype.getNormalisableRange()),
      displayName(parameterSpec.name)
{
    setWantsKeyboardFocus(false);
    valueText.setJustificationType(juce::Justification::centred);
    valueText.setFont(juce::FontOptions(11.0f));
    valueText.setColour(juce::Label::textColourId, colours::text);
    valueText.setInterceptsMouseClicks(false, false);

    switch (parameterSpec.kind)
    {
    case ParamKind::Choice:
        for (int i = 0; i < parameterSpec.choices.size(); ++i)
            choice.addItem(parameterSpec.choices[i], i + 1);
        choice.setWantsKeyboardFocus(false);
        choice.onChange = [this] {
            if (!updating && choice.getSelectedId() > 0)
            {
                session.undoManager().beginNewTransaction(parameterSpec.name);
                setValue(choice.getSelectedId() - 1);
            }
        };
        addAndMakeVisible(choice);
        break;
    case ParamKind::Bool:
        toggle.setButtonText(parameterSpec.name);
        toggle.setClickingTogglesState(false);
        toggle.setColour(juce::TextButton::buttonOnColourId, colours::play.darker(0.3f));
        toggle.setWantsKeyboardFocus(false);
        toggle.onClick = [this] {
            session.undoManager().beginNewTransaction(parameterSpec.name);
            setValue(documentValue() > 0.5 ? 0.0 : 1.0);
        };
        addAndMakeVisible(toggle);
        break;
    case ParamKind::Float:
    case ParamKind::Int:
        // The knob moves through the parameter's own normalised range, so its feel (the skew)
        // matches what controllers will get.
        knob.setRange(0.0, 1.0, 0.0);
        knob.setDoubleClickReturnValue(true, range.convertTo0to1(parameterSpec.defaultValue));
        knob.setWantsKeyboardFocus(false);
        knob.setPopupMenuEnabled(false);
        knob.onDragStart = [this] {
            session.undoManager().beginNewTransaction(parameterSpec.name);
        };
        knob.onValueChange = [this] {
            if (!updating)
                setValue(range.convertFrom0to1(static_cast<float>(knob.getValue())));
        };
        knob.addMouseListener(this, false); // right-click reaches mouseDown()
        addAndMakeVisible(knob);
        addAndMakeVisible(valueText);
        break;
    }
    refresh();
}

double ParameterControl::documentValue() const
{
    return LiveSet::deviceParameter(device, parameterSpec.id, parameterSpec.defaultValue);
}

void ParameterControl::setValue(double plain)
{
    const auto v = parameterSpec.constrain(static_cast<float>(plain));
    if (static_cast<float>(documentValue()) != v)
        session.set().setDeviceParameter(device, parameterSpec.id, v);
}

void ParameterControl::refresh()
{
    const auto v = static_cast<float>(documentValue());
    const juce::ScopedValueSetter<bool> guard(updating, true);
    switch (parameterSpec.kind)
    {
    case ParamKind::Choice:
        choice.setSelectedId(juce::roundToInt(v) + 1, juce::dontSendNotification);
        break;
    case ParamKind::Bool:
        toggle.setToggleState(v > 0.5f, juce::dontSendNotification);
        break;
    case ParamKind::Float:
    case ParamKind::Int:
        if (!knob.isMouseButtonDown())
            knob.setValue(range.convertTo0to1(v), juce::dontSendNotification);
        valueText.setText(parameterSpec.format(v), juce::dontSendNotification);
        break;
    }
}

void ParameterControl::paint(juce::Graphics& g)
{
    if (parameterSpec.kind == ParamKind::Bool)
        return;
    g.setColour(colours::dimText);
    g.setFont(juce::FontOptions(11.0f));
    g.drawFittedText(displayName, getLocalBounds().removeFromTop(14), juce::Justification::centred, 1);
}

void ParameterControl::resized()
{
    auto area = getLocalBounds();
    switch (parameterSpec.kind)
    {
    case ParamKind::Choice:
        area.removeFromTop(16);
        choice.setBounds(area.removeFromTop(22).reduced(2, 0));
        break;
    case ParamKind::Bool:
        toggle.setBounds(area.withSizeKeepingCentre(area.getWidth() - 4, 24));
        break;
    case ParamKind::Float:
    case ParamKind::Int:
        area.removeFromTop(14);
        valueText.setBounds(area.removeFromBottom(16));
        knob.setBounds(area.reduced(4, 0));
        break;
    }
}

void ParameterControl::mouseDown(const juce::MouseEvent& e)
{
    if (e.mods.isPopupMenu())
        showMenu();
}

void ParameterControl::showMenu()
{
    juce::PopupMenu menu;
    menu.addSectionHeader(parameterSpec.name);
    menu.addItem("Reset to default", [this] {
        session.undoManager().beginNewTransaction("Reset " + parameterSpec.name);
        setValue(parameterSpec.defaultValue);
    });
    menu.addItem("Type a value...", [safe = juce::Component::SafePointer<ParameterControl>(this)] {
        if (safe == nullptr)
            return;
        askForText(safe->parameterSpec.name, "Value", safe->parameterSpec.format(static_cast<float>(safe->documentValue())),
                   [safe](const juce::String& text) {
                       if (safe == nullptr)
                           return;
                       safe->session.undoManager().beginNewTransaction(safe->parameterSpec.name);
                       safe->setValue(safe->parameterSpec.parse(text));
                   });
    });
    if (extendMenu)
        extendMenu(menu);
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
}

} // namespace lattice::debugui
