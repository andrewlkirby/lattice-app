#pragma once

#include "core/model/LiveSet.h"
#include "devices/framework/ParameterSpec.h"

#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <memory>

namespace lattice::ui
{

/** A widget's link to one value in the document (docs/05-m4-ui.md, "Widget library"): a device
    parameter or a model property such as a track's volume. The ParameterSpec describes the value
    (range, skew, default, units, text), so every widget, the info view and Find read the same
    description.

    Edits go through the document's UndoManager. A gesture (a drag, from mouse down to mouse up) is
    one undo step; a single change outside a gesture (a reset, a typed value, a key press) is one
    step of its own. onChange is called whenever the value changes in the document, from the
    widget, from undo, or from anywhere else. Message thread only. */
class ValueBinding final : private juce::ValueTree::Listener
{
public:
    using Getter = std::function<double()>;
    using Setter = std::function<void(double)>;

    /** watched: the tree whose changes can change the value. matches: whether a property change
        on a tree inside it is this value's (an empty function: any change). */
    ValueBinding(ParameterSpec spec, Getter get, Setter set, juce::UndoManager* undo, juce::ValueTree watched,
                 std::function<bool(const juce::ValueTree&, const juce::Identifier&)> matches);
    ~ValueBinding() override;

    /** One parameter of a device in the set (its node in a chain). */
    static std::unique_ptr<ValueBinding> deviceParameter(LiveSet& set, juce::ValueTree device, ParameterSpec spec);
    /** A property of a model node, written through `set` (the LiveSet setter, so the model's own
        rules apply) and read from the node. */
    static std::unique_ptr<ValueBinding> property(ParameterSpec spec, juce::UndoManager* undo, juce::ValueTree node,
                                                  juce::Identifier id, Setter set);

    const ParameterSpec& spec() const noexcept { return parameterSpec; }

    float value() const;                 // plain, in the spec's units
    float normalised() const;            // 0..1 along the spec's (skewed) range
    float defaultNormalised() const;
    juce::String text() const;           // the value as shown ("1.20 kHz")
    juce::String textFor(float plain) const;
    float toNormalised(float plain) const;
    float fromNormalised(float normalisedValue) const;

    void beginGesture();
    void endGesture();
    bool inGesture() const noexcept { return gesture; }

    /** Sets the value (constrained to the spec). Outside a gesture this is one undo step. */
    void setValue(float plain);
    void setNormalised(float normalisedValue);
    void resetToDefault();
    /** Parses typed text ("2.5k", "-6 dB", a choice name) and sets it as one undo step. */
    void setFromText(const juce::String& typed);

    std::function<void()> onChange;

private:
    void write(float plain);
    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override;
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { changed(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { changed(); }
    void valueTreeRedirected(juce::ValueTree&) override { changed(); }
    void changed();

    const ParameterSpec parameterSpec;
    const juce::NormalisableRange<float> range;
    Getter get;
    Setter set;
    juce::UndoManager* undo;
    juce::ValueTree watched;
    std::function<bool(const juce::ValueTree&, const juce::Identifier&)> matches;
    bool gesture = false;
    float lastValue = 0.0f;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ValueBinding)
};

} // namespace lattice::ui
