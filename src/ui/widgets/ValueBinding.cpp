#include "ui/widgets/ValueBinding.h"

#include "core/model/UndoNames.h"
#include "devices/framework/DeviceRegistry.h"

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
    location = [node = watched, group = parameterSpec.group] {
        return undoNames::locationOf(node, DeviceRegistry::builtin(), group);
    };
}

ValueBinding::~ValueBinding()
{
    watched.removeListener(this);
}

juce::String ValueBinding::targetOf(const juce::ValueTree& node, const juce::Identifier& property)
{
    if (node.hasType(ids::master))
        return "master/" + property.toString();
    const auto id = LiveSet::idOf(node);
    return id != 0 ? "node:" + juce::String(id) + "/" + property.toString() : juce::String();
}

std::unique_ptr<ValueBinding> ValueBinding::deviceParameter(LiveSet& liveSet, juce::ValueTree device, ParameterSpec spec)
{
    const auto id = spec.id;
    const auto fallback = static_cast<double>(spec.defaultValue);
    auto* setPtr = &liveSet;
    auto binding = std::make_unique<ValueBinding>(
        std::move(spec), [device, id, fallback] { return LiveSet::deviceParameter(device, id, fallback); },
        [setPtr, device, id](double v) { setPtr->setDeviceParameter(device, id, v); }, liveSet.undoManager(), device,
        [device, id](const juce::ValueTree& tree, const juce::Identifier& property) {
            return tree.hasType(ids::params) && tree.getParent() == device && property.toString() == id;
        });
    binding->target = "device:" + juce::String(LiveSet::idOf(device)) + "/" + id;
    return binding;
}

std::unique_ptr<ValueBinding> ValueBinding::property(ParameterSpec spec, juce::UndoManager* undoManager,
                                                     juce::ValueTree node, juce::Identifier id, Setter setter)
{
    const auto fallback = static_cast<double>(spec.defaultValue);
    auto binding = std::make_unique<ValueBinding>(
        std::move(spec), [node, id, fallback] { return static_cast<double>(node.getProperty(id, fallback)); },
        std::move(setter), undoManager, node,
        [node, id](const juce::ValueTree& tree, const juce::Identifier& property) { return tree == node && property == id; });
    binding->target = targetOf(node, id);
    return binding;
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

juce::String ValueBinding::undoName(float plain, const juce::String& action) const
{
    const auto what = action.isNotEmpty() ? action : parameterSpec.name;
    return undoNames::make(undoNames::withValue(what, textFor(plain)), location ? location() : juce::String());
}

void ValueBinding::beginGesture()
{
    if (gesture)
        return;
    gesture = true;
    if (undo == nullptr)
        return;
    // Where it is is fixed now; the value is added as it moves (write()).
    gestureLocation = location ? location() : juce::String();
    gestureName = undoNames::make(parameterSpec.name, gestureLocation);
    undo->beginNewTransaction(gestureName);
}

void ValueBinding::endGesture()
{
    gesture = false;
    gestureLocation = {};
    gestureName = {};
}

void ValueBinding::write(float plain)
{
    const float v = parameterSpec.constrain(plain);
    if (v == value())
        return;
    set(static_cast<double>(v));
    // Just after the write the gesture's step is the current one, if the write made it (a step
    // begun by someone else since, or a write that changed nothing, keeps its own name).
    if (gesture && undo != nullptr && undo->getCurrentTransactionName() == gestureName)
    {
        gestureName = undoNames::make(undoNames::withValue(parameterSpec.name, textFor(v)), gestureLocation);
        undo->setCurrentTransactionName(gestureName);
    }
}

void ValueBinding::setValue(float plain)
{
    if (!gesture && undo != nullptr)
        undo->beginNewTransaction(undoName(plain));
    write(plain);
}

void ValueBinding::setNormalised(float normalisedValue)
{
    setValue(fromNormalised(normalisedValue));
}

void ValueBinding::resetToDefault()
{
    if (undo != nullptr)
        undo->beginNewTransaction(undoName(parameterSpec.defaultValue, "Reset " + parameterSpec.name));
    write(parameterSpec.defaultValue);
}

void ValueBinding::setFromText(const juce::String& typed)
{
    const float plain = parameterSpec.parse(typed);
    if (undo != nullptr)
        undo->beginNewTransaction(undoName(plain));
    write(plain);
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
