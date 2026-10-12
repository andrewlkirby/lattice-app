#include "devices/framework/DeviceParameter.h"

#include <cmath>

namespace lattice
{
namespace
{
juce::NormalisableRange<float> makeRange(const ParameterSpec& s)
{
    juce::NormalisableRange<float> r(s.minValue, s.maxValue, s.interval);
    if (s.kind == ParamKind::Float && !std::isnan(s.centreValue) && s.centreValue > s.minValue && s.centreValue < s.maxValue)
        r.setSkewForCentre(s.centreValue);
    return r;
}
} // namespace

DeviceParameter::DeviceParameter(ParameterSpec spec, int index)
    : juce::RangedAudioParameter(juce::ParameterID(spec.id, 1), spec.name),
      parameterSpec(std::move(spec)),
      parameterIndex(index),
      range(makeRange(parameterSpec)),
      baseValue(parameterSpec.constrain(parameterSpec.defaultValue))
{
}

juce::String DeviceParameter::getText(float normalised, int maximumLength) const
{
    auto text = parameterSpec.format(range.convertFrom0to1(normalised));
    return maximumLength > 0 ? text.substring(0, maximumLength) : text;
}

float DeviceParameter::getValueForText(const juce::String& text) const
{
    return range.convertTo0to1(parameterSpec.parse(text));
}

int DeviceParameter::getNumSteps() const
{
    if (!parameterSpec.isDiscrete())
        return juce::AudioProcessor::getDefaultNumParameterSteps();
    const float step = parameterSpec.interval > 0.0f ? parameterSpec.interval : 1.0f;
    return juce::jmax(2, juce::roundToInt((parameterSpec.maxValue - parameterSpec.minValue) / step) + 1);
}

juce::StringArray DeviceParameter::getAllValueStrings() const
{
    if (parameterSpec.kind == ParamKind::Choice)
        return parameterSpec.choices;
    if (parameterSpec.kind == ParamKind::Bool)
        return {"Off", "On"};
    return {};
}

} // namespace lattice
