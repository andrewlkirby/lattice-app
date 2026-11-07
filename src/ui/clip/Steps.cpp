#include "ui/clip/Steps.h"

#include <algorithm>
#include <cmath>

namespace lattice::ui::steps
{

namespace
{
constexpr double epsilon = 1.0e-9;

/** How far past its line a step's notes may start: just under half a step (half a step is the
    next step's). */
constexpr double nudgeReach = 0.49;
} // namespace

int stepOf(double start, double stepLength) noexcept
{
    return stepLength > 0.0 ? static_cast<int>(std::floor(start / stepLength + 0.5 + epsilon)) : 0;
}

notes::Indices notesIn(const notes::Notes& notes, Step step, double stepLength)
{
    notes::Indices out;
    for (std::size_t i = 0; i < notes.size(); ++i)
        if (notes[i].pitch == step.pitch && stepOf(notes[i].start, stepLength) == step.index)
            out.push_back(i);
    return out;
}

notes::Indices notesIn(const notes::Notes& notes, const std::vector<Step>& steps, double stepLength)
{
    notes::Indices out;
    for (std::size_t i = 0; i < notes.size(); ++i)
    {
        const Step at{notes[i].pitch, stepOf(notes[i].start, stepLength)};
        if (std::find(steps.begin(), steps.end(), at) != steps.end())
            out.push_back(i);
    }
    return out;
}

std::vector<State> row(const notes::Notes& notes, int pitch, double stepLength, int first, int count)
{
    std::vector<State> states(static_cast<std::size_t>(std::max(0, count)), State::off);
    if (stepLength <= 0.0)
        return states;
    for (const auto& n : notes)
    {
        if (n.pitch != pitch)
            continue;
        const int k = stepOf(n.start, stepLength) - first;
        if (k >= 0 && k < count)
        {
            auto& s = states[static_cast<std::size_t>(k)];
            if (!n.muted)
                s = State::on;
            else if (s != State::on)
                s = State::muted;
        }
        // The steps whose lines it sounds through, after its own.
        const double end = n.start + n.length;
        for (int j = std::max(k + 1, 0); j < count && (first + j) * stepLength < end - epsilon; ++j)
            if (states[static_cast<std::size_t>(j)] == State::off)
                states[static_cast<std::size_t>(j)] = State::tied;
    }
    return states;
}

notes::Change toggle(const notes::Notes& notes, Step step, double stepLength, int velocity)
{
    notes::Change change;
    change.removed = notesIn(notes, step, stepLength);
    if (change.removed.empty() && step.index >= 0 && step.pitch >= 0 && step.pitch < 128)
        change.added.push_back({step.pitch, step.index * stepLength, stepLength, std::clamp(velocity, 1, 127), false});
    return change;
}

const char* fieldName(Field field) noexcept
{
    switch (field)
    {
    case Field::velocity: return "velocity";
    case Field::length: return "length";
    case Field::nudge: return "nudge";
    }
    return "velocity";
}

std::optional<Field> fieldFromName(juce::StringRef name) noexcept
{
    for (const auto f : {Field::velocity, Field::length, Field::nudge})
        if (juce::String(name) == fieldName(f))
            return f;
    return std::nullopt;
}

juce::String target(const juce::ValueTree& clip, const std::vector<Step>& held, double stepLength, Field field)
{
    juce::StringArray list;
    for (const auto& s : held)
        list.add(juce::String(s.pitch) + ":" + juce::String(s.index));
    return "notes:" + juce::String(LiveSet::idOf(clip)) + "/" + juce::String(stepLength, 6) + "/" + list.joinIntoString(",") + "/" +
           fieldName(field);
}

std::unique_ptr<ValueBinding> binding(LiveSet& set, juce::UndoManager* undo, const juce::ValueTree& clip, std::vector<Step> held,
                                      double stepLength, Field field)
{
    if (!clip.isValid() || held.empty() || stepLength <= 0.0)
        return nullptr;
    const auto onSteps = [clip, held, stepLength] {
        const auto all = LiveSet::notesOf(clip);
        return std::pair{all, notesIn(all, held, stepLength)};
    };
    if (onSteps().second.empty())
        return nullptr;

    const auto specFor = [stepLength](Field f) -> ParameterSpec {
        switch (f)
        {
        case Field::length:
            return ParameterSpec::continuous("stepLength", "Length", static_cast<float>(notes::minLength), static_cast<float>(stepLength * 64.0),
                                             static_cast<float>(stepLength), ParamUnit::Beats)
                .withCentre(static_cast<float>(stepLength * 4.0))
                .shownScaled(1.0 / stepLength, " steps", 2)
                .inGroup("Step");
        case Field::nudge:
            return ParameterSpec::continuous("stepNudge", "Nudge", static_cast<float>(-0.5 * stepLength), static_cast<float>(nudgeReach * stepLength),
                                             0.0f, ParamUnit::Beats)
                .shownScaled(100.0 / stepLength, " %")
                .inGroup("Step");
        case Field::velocity: break;
        }
        return ParameterSpec::integer("stepVelocity", "Velocity", 1, 127, defaultVelocity).inGroup("Step");
    };
    auto spec = specFor(field);

    const auto get = [onSteps, stepLength, field]() -> double {
        const auto [all, on] = onSteps();
        if (on.empty())
            return field == Field::velocity ? defaultVelocity : field == Field::length ? stepLength : 0.0;
        const auto& n = all[on.front()];
        switch (field)
        {
        case Field::velocity: return n.velocity;
        case Field::length: return n.length;
        case Field::nudge: return n.start - stepOf(n.start, stepLength) * stepLength;
        }
        return 0.0;
    };
    const auto put = [&set, clip, onSteps, stepLength, field](double v) {
        const auto [all, on] = onSteps();
        notes::Change change;
        for (const auto i : on)
        {
            auto n = all[i];
            switch (field)
            {
            case Field::velocity: n.velocity = std::clamp(static_cast<int>(std::lround(v)), 1, 127); break;
            case Field::length: n.length = std::max(notes::minLength, v); break;
            case Field::nudge: n.start = std::max(0.0, stepOf(n.start, stepLength) * stepLength + v); break;
            }
            if (!(n == all[i]))
                change.changed.emplace_back(i, n);
        }
        notes::apply(set, clip, change);
    };
    auto b = std::make_unique<ValueBinding>(std::move(spec), get, put, undo, clip, nullptr);
    b->target = target(clip, held, stepLength, field);
    return b;
}

std::unique_ptr<ValueBinding> bindingFor(LiveSet& set, juce::UndoManager* undo, const juce::String& address)
{
    const auto parts = juce::StringArray::fromTokens(address, "/", "");
    if (parts.size() != 4)
        return nullptr;
    const auto clip = set.clipById(parts[0].getLargeIntValue());
    const double stepLength = parts[1].getDoubleValue();
    const auto field = fieldFromName(parts[3]);
    if (!clip.isValid() || stepLength <= 0.0 || !field)
        return nullptr;
    std::vector<Step> held;
    for (const auto& item : juce::StringArray::fromTokens(parts[2], ",", ""))
        if (item.containsChar(':'))
            held.push_back({item.upToFirstOccurrenceOf(":", false, false).getIntValue(), item.fromFirstOccurrenceOf(":", false, false).getIntValue()});
    return binding(set, undo, clip, std::move(held), stepLength, *field);
}

} // namespace lattice::ui::steps
