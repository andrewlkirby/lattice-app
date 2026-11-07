#include "control/surface/EncoderBank.h"

#include "ui/mapping/Targets.h"

#include <cmath>

namespace lattice::control
{

DisplayCell cellFor(const ui::ValueBinding& b)
{
    const auto& spec = b.spec();
    DisplayCell cell;
    cell.name = spec.name;
    cell.value = b.text();
    cell.position = b.normalised();
    cell.bipolar = spec.kind == ParamKind::Float && spec.minValue < 0.0f && spec.maxValue > 0.0f;
    cell.detail = b.textFor(spec.minValue) + juce::String::fromUTF8(" \xe2\x80\x93 ") + b.textFor(spec.maxValue);
    return cell;
}

EncoderBank::EncoderBank(LiveSet& set, DeviceSpecs& s, juce::UndoManager* u) : liveSet(set), specs(s), undo(u) {}

EncoderBank::~EncoderBank()
{
    endGestures();
}

EncoderBank::Encoder* EncoderBank::at(int encoder)
{
    return encoder >= 0 && encoder < static_cast<int>(encoders.size()) ? &encoders[static_cast<std::size_t>(encoder)] : nullptr;
}

const EncoderBank::Encoder* EncoderBank::at(int encoder) const
{
    return encoder >= 0 && encoder < static_cast<int>(encoders.size()) ? &encoders[static_cast<std::size_t>(encoder)] : nullptr;
}

bool EncoderBank::valid(const Encoder& e) const
{
    // Still in the set: the device or track may have gone since, or another set been loaded.
    if (e.binding == nullptr)
        return false;
    const auto& watched = e.binding->watchedTree();
    return watched == liveSet.state() || watched.isAChildOf(liveSet.state());
}

void EncoderBank::assign(const std::vector<juce::String>& targets)
{
    if (encoders.size() < targets.size())
        encoders.resize(targets.size());
    for (std::size_t i = 0; i < encoders.size(); ++i)
    {
        auto& e = encoders[i];
        const auto& wanted = i < targets.size() ? targets[i] : juce::String();
        if (wanted == e.target && (wanted.isEmpty() || valid(e)))
            continue;
        end(e);
        e.target = wanted;
        e.binding = wanted.isEmpty() ? nullptr : ui::bindingForTarget(liveSet, specs, undo, wanted);
        e.position = e.written = e.absoluteLast = -1.0;
        e.picked = false;
    }
}

const juce::String& EncoderBank::target(int encoder) const
{
    static const juce::String none;
    const auto* e = at(encoder);
    return e != nullptr ? e->target : none;
}

ui::ValueBinding* EncoderBank::binding(int encoder) const
{
    const auto* e = at(encoder);
    return e != nullptr && valid(*e) ? e->binding.get() : nullptr;
}

int EncoderBank::detents(const ParameterSpec& spec)
{
    if (spec.interval <= 0.0f)
        return 0;
    const int settings = static_cast<int>(std::floor((spec.maxValue - spec.minValue) / spec.interval + 0.5f)) + 1;
    return settings > 1 && settings <= detentSettings ? settings : 0;
}

double EncoderBank::stepSize(const ParameterSpec& spec, int resolution, bool fine)
{
    if (const int settings = detents(spec); settings > 0)
        return 1.0 / (static_cast<double>(settings - 1) * stepsPerSetting);
    return 1.0 / (static_cast<double>(std::max(1, resolution)) * (fine ? 10.0 : 1.0));
}

void EncoderBank::begin(Encoder& e)
{
    if (e.binding != nullptr && !e.binding->inGesture())
    {
        e.binding->beginGesture();
        e.position = -1.0;
    }
}

void EncoderBank::end(Encoder& e)
{
    if (e.binding != nullptr && e.binding->inGesture())
        e.binding->endGesture();
    e.position = -1.0;
}

void EncoderBank::moveTo(Encoder& e, double position)
{
    e.position = juce::jlimit(0.0, 1.0, position);
    e.binding->setNormalised(static_cast<float>(e.position));
    e.written = e.binding->normalised();
    e.movedAt = juce::Time::getMillisecondCounter();
    if (!e.touched && !isTimerRunning())
        startTimer(restMs / 4);
}

void EncoderBank::turn(int encoder, int steps, int resolution, bool fine)
{
    auto* e = at(encoder);
    if (e == nullptr || steps == 0 || !valid(*e))
        return;
    begin(*e);
    const double now = e->binding->normalised();
    // This gesture's own moves go on from where they left the value (keeping what's less than a
    // setting); anything else (a new gesture, a change from elsewhere) starts from where it is.
    const bool continuing = e->position >= 0.0 && std::abs(now - e->written) < 1.0e-6;
    const auto& spec = e->binding->spec();
    if (detents(spec) > 0)
    {
        // A setting every stepsPerSetting steps, never sooner (rounding would move it half way).
        if (!continuing)
        {
            e->detentFrom = e->binding->value();
            e->detentSteps = 0;
        }
        e->detentSteps += steps;
        e->binding->setValue(spec.constrain(e->detentFrom + static_cast<float>(e->detentSteps / stepsPerSetting) * spec.interval));
        e->written = e->position = e->binding->normalised();
        e->movedAt = juce::Time::getMillisecondCounter();
        if (!e->touched && !isTimerRunning())
            startTimer(restMs / 4);
        return;
    }
    moveTo(*e, (continuing ? e->position : now) + steps * stepSize(spec, resolution, fine));
}

void EncoderBank::set(int encoder, float position)
{
    auto* e = at(encoder);
    if (e == nullptr || !valid(*e))
        return;
    const double want = juce::jlimit(0.0, 1.0, static_cast<double>(position));
    const double v = e->binding->normalised();
    if (!e->picked)
    {
        const double was = e->absoluteLast >= 0.0 ? e->absoluteLast : want;
        e->picked = std::abs(want - v) <= 1.5 / 127.0 || (was - v) * (want - v) < 0.0;
    }
    e->absoluteLast = want;
    if (!e->picked)
        return;
    begin(*e);
    moveTo(*e, want);
}

void EncoderBank::touch(int encoder)
{
    if (auto* e = at(encoder))
    {
        e->touched = true;
        if (valid(*e))
            begin(*e);
    }
}

void EncoderBank::release(int encoder)
{
    if (auto* e = at(encoder))
    {
        e->touched = false;
        e->picked = false; // an absolute control picks up again next time
        end(*e);
    }
}

bool EncoderBank::isTouched(int encoder) const
{
    const auto* e = at(encoder);
    return e != nullptr && e->touched;
}

int EncoderBank::touchedEncoder() const
{
    for (std::size_t i = 0; i < encoders.size(); ++i)
        if (encoders[i].touched)
            return static_cast<int>(i);
    return -1;
}

void EncoderBank::endGestures()
{
    stopTimer();
    for (auto& e : encoders)
        end(e);
}

void EncoderBank::timerCallback()
{
    // An encoder without a touch at rest has ended its turn: the turn so far is one undo step.
    const auto now = juce::Time::getMillisecondCounter();
    bool open = false;
    for (auto& e : encoders)
        if (!e.touched && e.binding != nullptr && e.binding->inGesture())
        {
            if (now - e.movedAt >= static_cast<juce::uint32>(restMs))
                end(e);
            else
                open = true;
        }
    if (!open)
        stopTimer();
}

} // namespace lattice::control
