#pragma once

#include <juce_core/juce_core.h>

#include <cmath>
#include <cstdint>
#include <functional>
#include <utility>

namespace lattice
{

enum class ParamKind : std::uint8_t
{
    Float,
    Int,
    Bool,
    Choice
};

enum class ParamUnit : std::uint8_t
{
    None,
    Hz,
    Decibels,
    Milliseconds,
    Seconds,
    Percent,
    Cents,
    Semitones,
    TableSteps,
    Ratio,
    Beats
};

/** One parameter of a device (docs/04-m3-devices.md, "Parameters"). The ID is stable forever once
    released: saved sets, presets, macro mappings, controller mappings and (later) automation all
    refer to it. The group decides which section and controller page the parameter appears on;
    within a group, parameters keep their declaration order (most important first).

    Values are "plain" values in the parameter's own units (Hz, dB, ...), which is also how they are
    saved, so changing a range never changes a saved sound. */
struct ParameterSpec
{
    using Formatter = std::function<juce::String(float plainValue)>;

    juce::String id;
    juce::String name;
    juce::String group;
    ParamKind kind = ParamKind::Float;
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float defaultValue = 0.0f;
    float centreValue = 0.0f; // > min: the value at the middle of a knob's travel (skewed range)
    float interval = 0.0f;    // 0 = continuous
    ParamUnit unit = ParamUnit::None;
    juce::StringArray choices; // Choice parameters: one name per value 0..n-1
    bool modulatable = true;   // macros and (later) automation may drive it
    Formatter formatter;       // optional; the default shows the value with its unit

    // --- Builders ---------------------------------------------------------------------------------
    static ParameterSpec continuous(juce::String id, juce::String name, float min, float max, float def,
                                    ParamUnit unit = ParamUnit::None)
    {
        ParameterSpec s;
        s.id = std::move(id);
        s.name = std::move(name);
        s.kind = ParamKind::Float;
        s.minValue = min;
        s.maxValue = max;
        s.defaultValue = def;
        s.unit = unit;
        return s;
    }

    static ParameterSpec integer(juce::String id, juce::String name, int min, int max, int def,
                                 ParamUnit unit = ParamUnit::None)
    {
        auto s = continuous(std::move(id), std::move(name), static_cast<float>(min), static_cast<float>(max),
                            static_cast<float>(def), unit);
        s.kind = ParamKind::Int;
        s.interval = 1.0f;
        return s;
    }

    static ParameterSpec toggle(juce::String id, juce::String name, bool def)
    {
        auto s = continuous(std::move(id), std::move(name), 0.0f, 1.0f, def ? 1.0f : 0.0f);
        s.kind = ParamKind::Bool;
        s.interval = 1.0f;
        return s;
    }

    static ParameterSpec choice(juce::String id, juce::String name, juce::StringArray options, int def)
    {
        auto s = continuous(std::move(id), std::move(name), 0.0f,
                            static_cast<float>(juce::jmax(0, options.size() - 1)), static_cast<float>(def));
        s.kind = ParamKind::Choice;
        s.interval = 1.0f;
        s.choices = std::move(options);
        return s;
    }

    ParameterSpec&& inGroup(juce::String g) &&
    {
        group = std::move(g);
        return std::move(*this);
    }
    /** Skews the range so that `centre` sits at the middle of the knob (e.g. 1 kHz for a cutoff). */
    ParameterSpec&& withCentre(float centre) &&
    {
        centreValue = centre;
        return std::move(*this);
    }
    ParameterSpec&& withInterval(float step) &&
    {
        interval = step;
        return std::move(*this);
    }
    ParameterSpec&& withFormatter(Formatter f) &&
    {
        formatter = std::move(f);
        return std::move(*this);
    }
    ParameterSpec&& notModulatable() &&
    {
        modulatable = false;
        return std::move(*this);
    }

    bool isDiscrete() const noexcept { return kind != ParamKind::Float || interval > 0.0f; }

    /** Clamps (and for discrete parameters rounds) a plain value into the range. */
    float constrain(float plain) const noexcept
    {
        float v = juce::jlimit(minValue, maxValue, plain);
        if (interval > 0.0f)
            v = juce::jlimit(minValue, maxValue, minValue + interval * std::round((v - minValue) / interval));
        return v;
    }

    /** The default text for a plain value: the value with its unit. */
    juce::String format(float plain) const;

    /** Parses text typed by the user (e.g. "2.5k", "-6 dB", a choice name). Returns the plain value,
        constrained; falls back to the default for unreadable text. */
    float parse(const juce::String& text) const;

    /** A valid ID: starts with a letter; letters, digits, '_' and '.' only (it is also an XML
        attribute name in saved sets). */
    static bool isValidId(const juce::String& id);
};

} // namespace lattice
