#include "devices/midi/Arpeggiator.h"

#include "devices/dsp/SyncRates.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace lattice
{
namespace
{
using P = ArpeggiatorDevice::Param;

std::vector<ParameterSpec> makeSpecs()
{
    std::vector<ParameterSpec> s(static_cast<std::size_t>(P::numParams));
    const auto set = [&](int index, ParameterSpec spec, const char* group) {
        spec.group = group;
        s[static_cast<std::size_t>(index)] = std::move(spec);
    };
    set(P::style, ParameterSpec::choice("arp.style", "Style", {"Up", "Down", "Up/Down", "Played", "Random", "Chord"}, 0),
        "Arpeggiator");
    set(P::rate, ParameterSpec::choice("arp.rate", "Rate", dsp::SyncRates::names(), dsp::SyncRates::indexOf("1/16")),
        "Arpeggiator");
    set(P::gate,
        ParameterSpec::continuous("arp.gate", "Gate", 0.01f, 2.0f, 0.5f, ParamUnit::Percent).withFormatter([](float v) {
            return juce::String(juce::roundToInt(v * 100.0f)) + " %";
        }),
        "Arpeggiator");
    set(P::range, ParameterSpec::integer("arp.range", "Ranges", 1, 8, 1), "Arpeggiator");
    set(P::distance, ParameterSpec::integer("arp.distance", "Distance", -48, 48, 12, ParamUnit::TableSteps),
        "Arpeggiator");
    set(P::transpose, ParameterSpec::integer("arp.transpose", "Transpose", -48, 48, 0, ParamUnit::TableSteps),
        "Arpeggiator");
    set(P::retrigger, ParameterSpec::choice("arp.retrigger", "Retrigger", {"Off", "Note", "Beat"}, 1), "Retrigger");
    set(P::retriggerRate,
        ParameterSpec::choice("arp.retriggerRate", "Retrigger Every", dsp::SyncRates::names(), dsp::SyncRates::indexOf("1 bar")),
        "Retrigger");
    set(P::hold, ParameterSpec::toggle("arp.hold", "Hold", false), "Retrigger");
    return s;
}

/** The offset (from the block's start) of the first sample at or after a beat, as the transport
    computes it, so arpeggio steps land exactly where clip notes on that beat would. */
int offsetOf(double beat, const DeviceTimeInfo& time) noexcept
{
    const double samples = (beat - time.beatAtStart) * time.samplesPerBeat;
    return static_cast<int>(std::max(0.0, std::ceil(samples - 1e-5)));
}
} // namespace

ArpeggiatorDevice::ArpeggiatorDevice() : StudioDevice(typeId, "Arpeggiator", DeviceKind::MidiEffect, makeSpecs())
{
}

void ArpeggiatorDevice::reset() noexcept
{
    numHeld = 0;
    numPlaying = 0;
    anyKeyDown = false;
    position = 0;
    restartPending = true;
    lastRetriggerBeat = -1.0;
    nextStepBeat = -1.0;
}

void ArpeggiatorDevice::noteOn(const TimedMidi& e) noexcept
{
    const bool holding = value(P::hold) >= 0.5f;
    if (holding && !anyKeyDown)
        numHeld = 0; // with hold, a new chord replaces the latched one
    if (numHeld == 0)
        restartPending = true;
    if (static_cast<Retrigger>(juce::roundToInt(value(P::retrigger))) == Retrigger::Note)
        restartPending = true;

    for (int i = 0; i < numHeld; ++i)
        if (held[static_cast<std::size_t>(i)].note == e.bytes[1])
        {
            held[static_cast<std::size_t>(i)].velocity = e.bytes[2];
            held[static_cast<std::size_t>(i)].keyDown = true;
            anyKeyDown = true;
            return;
        }
    if (numHeld < maxHeld)
        held[static_cast<std::size_t>(numHeld++)] = {e.bytes[1], e.bytes[2], static_cast<std::uint8_t>(e.bytes[0] & 0x0f), true};
    anyKeyDown = true;
}

void ArpeggiatorDevice::noteOff(const TimedMidi& e) noexcept
{
    const bool holding = value(P::hold) >= 0.5f;
    for (int i = 0; i < numHeld; ++i)
    {
        auto& h = held[static_cast<std::size_t>(i)];
        if (h.note != e.bytes[1])
            continue;
        h.keyDown = false;
        if (!holding)
        {
            std::copy(held.begin() + i + 1, held.begin() + numHeld, held.begin() + i);
            --numHeld;
        }
        break;
    }
    anyKeyDown = false;
    for (int i = 0; i < numHeld; ++i)
        anyKeyDown = anyKeyDown || held[static_cast<std::size_t>(i)].keyDown;
}

int ArpeggiatorDevice::buildPattern() noexcept
{
    const auto style = static_cast<Style>(juce::roundToInt(value(P::style)));
    const int ranges = juce::jlimit(1, 8, juce::roundToInt(value(P::range)));
    const int distance = juce::roundToInt(value(P::distance));
    const int shift = juce::roundToInt(value(P::transpose));

    // The held notes in the style's order (Played keeps the order they were played in).
    std::array<HeldNote, maxHeld> order{};
    std::copy(held.begin(), held.begin() + numHeld, order.begin());
    const auto byNote = [](const HeldNote& a, const HeldNote& b) { return a.note < b.note; };
    if (style != Style::Played)
        std::sort(order.begin(), order.begin() + numHeld, byNote);

    int count = 0;
    const auto add = [&](const HeldNote& h, int range) {
        const int note = h.note + range * distance + shift;
        if (note >= 0 && note <= 127 && count < maxPattern)
        {
            pattern[static_cast<std::size_t>(count)] = static_cast<std::int16_t>(note);
            patternVelocity[static_cast<std::size_t>(count)] = h.velocity;
            ++count;
        }
    };
    if (style == Style::Down)
    {
        for (int r = ranges - 1; r >= 0; --r)
            for (int i = numHeld - 1; i >= 0; --i)
                add(order[static_cast<std::size_t>(i)], r);
        return count;
    }
    for (int r = 0; r < ranges; ++r)
        for (int i = 0; i < numHeld; ++i)
            add(order[static_cast<std::size_t>(i)], r);
    if (style == Style::UpDown && count > 2)
    {
        // Back down without repeating the top and bottom notes.
        const int up = count;
        for (int i = up - 2; i >= 1 && count < maxPattern; --i)
        {
            pattern[static_cast<std::size_t>(count)] = pattern[static_cast<std::size_t>(i)];
            patternVelocity[static_cast<std::size_t>(count)] = patternVelocity[static_cast<std::size_t>(i)];
            ++count;
        }
    }
    return count;
}

void ArpeggiatorDevice::step(double beat, int offset, MidiBlock& out) noexcept
{
    const auto retrigger = static_cast<Retrigger>(juce::roundToInt(value(P::retrigger)));
    if (retrigger == Retrigger::Beat)
    {
        const double every = dsp::SyncRates::beatsAt(juce::roundToInt(value(P::retriggerRate)));
        const double cycle = std::floor(beat / every + 1e-9);
        if (cycle != lastRetriggerBeat)
        {
            lastRetriggerBeat = cycle;
            restartPending = true;
        }
    }
    if (numHeld == 0)
        return;
    const int count = buildPattern();
    if (count == 0)
        return;
    if (restartPending)
    {
        position = 0;
        restartPending = false;
    }

    const double stepBeats = dsp::SyncRates::beatsAt(juce::roundToInt(value(P::rate)));
    const double offBeat = beat + stepBeats * static_cast<double>(value(P::gate));
    const auto style = static_cast<Style>(juce::roundToInt(value(P::style)));
    const auto play = [&](int note, int velocity, int channel) {
        // Still sounding from an earlier step (gate over 100 %): end it first.
        for (int i = 0; i < numPlaying; ++i)
            if (playing[static_cast<std::size_t>(i)].note == note)
            {
                out.add(offset, static_cast<std::uint8_t>(0x80 | playing[static_cast<std::size_t>(i)].channel),
                        static_cast<std::uint8_t>(note), 0);
                playing[static_cast<std::size_t>(i)] = playing[static_cast<std::size_t>(--numPlaying)];
                break;
            }
        out.add(offset, static_cast<std::uint8_t>(0x90 | channel), static_cast<std::uint8_t>(note),
                static_cast<std::uint8_t>(std::max(1, velocity)));
        if (numPlaying < maxHeld)
            playing[static_cast<std::size_t>(numPlaying++)] = {static_cast<std::uint8_t>(note),
                                                              static_cast<std::uint8_t>(channel), offBeat};
    };

    const int channel = held[0].channel;
    if (style == Style::Chord)
    {
        // Every held note together, one range per step.
        const int ranges = std::max(1, count / std::max(1, numHeld));
        const int r = position % ranges;
        for (int i = 0; i < numHeld && r * numHeld + i < count; ++i)
            play(pattern[static_cast<std::size_t>(r * numHeld + i)], patternVelocity[static_cast<std::size_t>(r * numHeld + i)],
                 channel);
        ++position;
        return;
    }
    int index = position % count;
    if (style == Style::Random)
    {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        index = static_cast<int>(random % static_cast<std::uint32_t>(count));
    }
    play(pattern[static_cast<std::size_t>(index)], patternVelocity[static_cast<std::size_t>(index)], channel);
    position = (position + 1) % count;
}

void ArpeggiatorDevice::process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept
{
    const int n = io.numSamples;
    auto& out = *io.midiOut;
    const double stepBeats = dsp::SyncRates::beatsAt(juce::roundToInt(value(P::rate)));
    // The next step carries over from block to block, so a step right at a block boundary is never
    // lost or played twice. It is found afresh when the grid moved (a new rate, the transport
    // starting or jumping).
    if (nextStepBeat < 0.0 || stepBeats != lastStepBeats || nextStepBeat < time.beatAtStart - stepBeats - 1e-9 ||
        nextStepBeat > time.beatAtStart + stepBeats + 1e-9)
        nextStepBeat = std::ceil(time.beatAtStart / stepBeats - 1e-9) * stepBeats;
    lastStepBeats = stepBeats;
    double stepBeat = nextStepBeat;
    int nextInput = 0;
    const MidiBlock& in = *io.midiIn;
    constexpr int never = std::numeric_limits<int>::max();

    for (int guard = 0; guard < 100000; ++guard)
    {
        // The earliest of: a playing note's end, an input event, the next step (in that order on ties).
        int offAt = never;
        for (int i = 0; i < numPlaying; ++i)
            offAt = std::min(offAt, offsetOf(playing[static_cast<std::size_t>(i)].offBeat, time));
        if (offAt >= n)
            offAt = never;
        const int inAt = nextInput < in.count ? in.events[nextInput].sampleOffset : never;
        int stepAt = offsetOf(stepBeat, time);
        if (stepAt >= n)
            stepAt = never;
        const int at = std::min({offAt, inAt, stepAt});
        if (at == never)
            break;

        if (offAt == at)
        {
            for (int i = 0; i < numPlaying;)
            {
                auto& p = playing[static_cast<std::size_t>(i)];
                if (offsetOf(p.offBeat, time) <= at)
                {
                    out.add(at, static_cast<std::uint8_t>(0x80 | p.channel), p.note, 0);
                    p = playing[static_cast<std::size_t>(--numPlaying)];
                }
                else
                {
                    ++i;
                }
            }
            continue;
        }
        if (inAt == at)
        {
            const auto& e = in.events[nextInput++];
            if (isNoteOn(e))
                noteOn(e);
            else if (isNoteOff(e))
                noteOff(e);
            else
                out.add(e); // controllers, bend, pressure pass through
            continue;
        }
        step(stepBeat, stepAt, out);
        stepBeat += stepBeats;
    }
    nextStepBeat = stepBeat;
}

} // namespace lattice
