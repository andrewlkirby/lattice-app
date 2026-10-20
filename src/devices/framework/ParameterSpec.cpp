#include "devices/framework/ParameterSpec.h"

namespace lattice
{
namespace
{
juce::String trimmedNumber(double v, int decimals)
{
    auto s = juce::String(v, decimals);
    if (s.containsChar('.'))
    {
        s = s.trimCharactersAtEnd("0");
        if (s.endsWithChar('.'))
            s = s.dropLastCharacters(1);
    }
    if (s == "-0")
        s = "0";
    return s;
}

/** Three significant digits, at least none and at most two decimals. */
juce::String significant(double v)
{
    const double a = std::abs(v);
    const int decimals = a >= 100.0 ? 0 : (a >= 10.0 ? 1 : 2);
    return trimmedNumber(v, decimals);
}
} // namespace

juce::String ParameterSpec::format(float plain) const
{
    if (formatter)
        return formatter(plain);

    switch (kind)
    {
    case ParamKind::Bool:
        return plain >= 0.5f ? "On" : "Off";
    case ParamKind::Choice:
    {
        const int index = juce::jlimit(0, juce::jmax(0, choices.size() - 1), juce::roundToInt(plain));
        return choices[index];
    }
    case ParamKind::Int:
    case ParamKind::Float:
        break;
    }

    const double v = plain;
    switch (unit)
    {
    case ParamUnit::Hz:
        return v >= 1000.0 ? significant(v / 1000.0) + " kHz" : significant(v) + " Hz";
    case ParamUnit::Decibels:
        return v <= -69.95 && minValue <= -70.0f ? juce::String("-inf dB") : significant(v) + " dB";
    case ParamUnit::Milliseconds:
        return v >= 1000.0 ? significant(v / 1000.0) + " s" : significant(v) + " ms";
    case ParamUnit::Seconds:
        return v < 1.0 ? significant(v * 1000.0) + " ms" : significant(v) + " s";
    case ParamUnit::Percent:
        return trimmedNumber(v, std::abs(v) < 10.0 ? 1 : 0) + " %";
    case ParamUnit::Cents:
        return trimmedNumber(v, 1) + " ct";
    case ParamUnit::Semitones:
        return (v > 0.0 ? "+" : "") + trimmedNumber(v, kind == ParamKind::Int ? 0 : 2) + " st";
    case ParamUnit::TableSteps:
        return (v > 0.0 ? "+" : "") + trimmedNumber(v, kind == ParamKind::Int ? 0 : 2) + " steps";
    case ParamUnit::Ratio:
        return significant(v) + " : 1";
    case ParamUnit::Beats:
        return trimmedNumber(v, 3) + " beats";
    case ParamUnit::None:
        break;
    }
    return kind == ParamKind::Int ? juce::String(juce::roundToInt(v)) : significant(v);
}

float ParameterSpec::parse(const juce::String& text) const
{
    const auto t = text.trim();
    if (kind == ParamKind::Bool)
    {
        const auto lower = t.toLowerCase();
        if (lower == "on" || lower == "1" || lower == "true" || lower == "yes")
            return 1.0f;
        if (lower == "off" || lower == "0" || lower == "false" || lower == "no")
            return 0.0f;
        return defaultValue;
    }
    if (kind == ParamKind::Choice)
    {
        for (int i = 0; i < choices.size(); ++i)
            if (choices[i].equalsIgnoreCase(t))
                return static_cast<float>(i);
        if (t.containsOnly("0123456789") && t.isNotEmpty())
            return constrain(static_cast<float>(t.getIntValue()));
        return defaultValue;
    }

    if (t.containsIgnoreCase("inf") && unit == ParamUnit::Decibels)
        return minValue;

    // The leading number; any unit text after it only scales it.
    const auto first = t.trimCharactersAtStart("+-");
    if (first.isEmpty() || !(juce::CharacterFunctions::isDigit(first[0]) || first[0] == '.'))
        return defaultValue;
    double v = t.getDoubleValue();

    const auto lower = t.toLowerCase();
    const bool kilo = lower.contains("k") && !lower.contains("ms");
    if (unit == ParamUnit::Hz && kilo)
        v *= 1000.0;
    else if (unit == ParamUnit::Milliseconds && lower.endsWith("s") && !lower.endsWith("ms"))
        v *= 1000.0;
    else if (unit == ParamUnit::Seconds && lower.endsWith("ms"))
        v /= 1000.0;
    return constrain(static_cast<float>(v));
}

bool ParameterSpec::isValidId(const juce::String& candidate)
{
    if (candidate.isEmpty() || !juce::CharacterFunctions::isLetter(candidate[0]))
        return false;
    return candidate.containsOnly("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.");
}

} // namespace lattice
