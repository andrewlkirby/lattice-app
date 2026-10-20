#include "ui/widgets/ValueBinding.h"

namespace lattice::ui
{

ValueBinding::ValueBinding(ParameterSpec spec, Getter getter, Setter setter, juce::UndoManager* undoManager,
                           juce::ValueTree watchedTree,
                           std::function<bool(const juce::ValueTree&, const juce::Identifier&)> isThisValue)
    : parameterSpec(std::move(spec)),
      range(parameterSpec.normalisableRange()),
      get(std::move(getter)),
      set(std::move(setter)),
      undo(undoManager),
      watched(std::move(watchedTree)),
      matches(std::move(isThisValue))
{
    lastValue = value();
    watched.addListener(this);
}

ValueBinding::~ValueBinding()
{
    watched.removeListener(this);
}

std::unique_ptr<ValueBinding> ValueBinding::deviceParameter(LiveSet& liveSet, juce::ValueTree device, ParameterSpec spec)
{
    const auto id = spec.id;
    const auto fallback = static_cast<double>(spec.defaultValue);
    auto* setPtr = &liveSet;
    return std::make_unique<ValueBinding>(
        std::move(spec), [device, id, fallback] { return LiveSet::deviceParameter(device, id, fallback); },
        [setPtr, device, id](double v) { setPtr->setDeviceParameter(device, id, v); }, liveSet.undoManager(), device,
        [device, id](const juce::ValueTree& tree, const juce::Identifier& property) {
            return tree.hasType(ids::params) && tree.getParent() == device && property.toString() == id;
        });
}

std::unique_ptr<ValueBinding> ValueBinding::property(ParameterSpec spec, juce::UndoManager* undoManager,
                                                     juce::ValueTree node, juce::Identifier id, Setter setter)
{
    const auto fallback = static_cast<double>(spec.defaultValue);
    return std::make_unique<ValueBinding>(
        std::move(spec), [node, id, fallback] { return static_cast<double>(node.getProperty(id, fallback)); },
        std::move(setter), undoManager, node,
        [node, id](const juce::ValueTree& tree, const juce::Identifier& property) { return tree == node && property == id; });
}

float ValueBinding::value() const
{
    return parameterSpec.constrain(static_cast<float>(get()));
}

float ValueBinding::normalised() const
{
    return toNormalised(value());
}

float ValueBinding::defaultNormalised() const
{
    return toNormalised(parameterSpec.defaultValue);
}

juce::String ValueBinding::text() const
{
    return textFor(value());
}

juce::String ValueBinding::textFor(float plain) const
{
    return parameterSpec.format(parameterSpec.constrain(plain));
}

float ValueBinding::toNormalised(float plain) const
{
    return juce::jlimit(0.0f, 1.0f, range.convertTo0to1(parameterSpec.constrain(plain)));
}

float ValueBinding::fromNormalised(float normalisedValue) const
{
    return parameterSpec.constrain(range.convertFrom0to1(juce::jlimit(0.0f, 1.0f, normalisedValue)));
}

void ValueBinding::beginGesture()
{
    if (gesture)
        return;
    gesture = true;
    if (undo != nullptr)
        undo->beginNewTransaction(parameterSpec.name);
}

void ValueBinding::endGesture()
{
    gesture = false;
}

void ValueBinding::write(float plain)
{
    const float v = parameterSpec.constrain(plain);
    if (v != value())
        set(static_cast<double>(v));
}

void ValueBinding::setValue(float plain)
{
    if (!gesture && undo != nullptr)
        undo->beginNewTransaction(parameterSpec.name);
    write(plain);
}

void ValueBinding::setNormalised(float normalisedValue)
{
    setValue(fromNormalised(normalisedValue));
}

void ValueBinding::resetToDefault()
{
    if (undo != nullptr)
        undo->beginNewTransaction("Reset " + parameterSpec.name);
    write(parameterSpec.defaultValue);
}

void ValueBinding::setFromText(const juce::String& typed)
{
    if (undo != nullptr)
        undo->beginNewTransaction(parameterSpec.name);
    write(parameterSpec.parse(typed));
}

void ValueBinding::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    if (!matches || matches(tree, property))
        changed();
}

void ValueBinding::changed()
{
    // Only real changes: a device's tree sees every parameter's changes.
    const float v = value();
    if (v == lastValue)
        return;
    lastValue = v;
    if (onChange)
        onChange();
}

} // namespace lattice::ui
