#pragma once

#include "core/RealtimeCheck.h"
#include "devices/framework/ParameterSpec.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <atomic>

namespace lattice
{

/** A device parameter built from a ParameterSpec (docs/04-m3-devices.md, "Parameters").

    It holds two plain values, each an atomic:
    - the base value, which mirrors the document: the message thread sets it whenever the saved
      value changes (an edit, undo, a preset);
    - an optional override, which a rack macro (or later automation) sets on the audio thread while
      it controls the parameter.

    plain() returns the override when there is one and the base value otherwise. Reading costs two
    relaxed loads, so devices read every parameter once per block and smooth the result themselves.

    As a juce::RangedAudioParameter it also works with JUCE's parameter tools and with hosts (M6). */
class DeviceParameter final : public juce::RangedAudioParameter
{
public:
    DeviceParameter(ParameterSpec spec, int index);

    const ParameterSpec& spec() const noexcept { return parameterSpec; }
    const juce::String& id() const noexcept { return parameterSpec.id; }
    int index() const noexcept { return parameterIndex; }

    /** The value in effect (any thread). */
    float plain() const noexcept LATTICE_NONBLOCKING
    {
        return overridden.load(std::memory_order_relaxed) ? overrideValue.load(std::memory_order_relaxed)
                                                          : baseValue.load(std::memory_order_relaxed);
    }
    float base() const noexcept { return baseValue.load(std::memory_order_relaxed); }

    /** Message thread: the document's value changed. Constrained to the range. */
    void setBase(float plainValue) noexcept { baseValue.store(parameterSpec.constrain(plainValue)); }

    /** Audio thread (a macro): takes control of the value until clearOverride(). */
    void setOverride(float plainValue) noexcept LATTICE_NONBLOCKING
    {
        overrideValue.store(parameterSpec.constrain(plainValue), std::memory_order_relaxed);
        overridden.store(true, std::memory_order_relaxed);
    }
    void clearOverride() noexcept LATTICE_NONBLOCKING { overridden.store(false, std::memory_order_relaxed); }
    bool isOverridden() const noexcept { return overridden.load(std::memory_order_relaxed); }

    juce::String textFor(float plainValue) const { return parameterSpec.format(plainValue); }

    // juce::RangedAudioParameter
    const juce::NormalisableRange<float>& getNormalisableRange() const override { return range; }
    float getValue() const override { return range.convertTo0to1(plain()); }
    void setValue(float normalised) override { setBase(range.convertFrom0to1(normalised)); }
    float getDefaultValue() const override { return range.convertTo0to1(parameterSpec.defaultValue); }
    juce::String getText(float normalised, int maximumLength) const override;
    float getValueForText(const juce::String& text) const override;
    juce::String getLabel() const override { return {}; }
    int getNumSteps() const override;
    bool isDiscrete() const override { return parameterSpec.isDiscrete(); }
    bool isBoolean() const override { return parameterSpec.kind == ParamKind::Bool; }
    juce::StringArray getAllValueStrings() const override;

private:
    ParameterSpec parameterSpec;
    int parameterIndex;
    juce::NormalisableRange<float> range;
    std::atomic<float> baseValue;
    std::atomic<float> overrideValue{0.0f};
    std::atomic<bool> overridden{false};
};

} // namespace lattice
