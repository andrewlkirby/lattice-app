#include "devices/midi/Chord.h"

namespace lattice
{
namespace
{
std::vector<ParameterSpec> makeSpecs()
{
    std::vector<ParameterSpec> s;
    for (int i = 1; i <= ChordDevice::numShifts; ++i)
    {
        const juce::String n(i);
        s.push_back(ParameterSpec::integer("chord.shift" + n, "Shift " + n, -36, 36, 0, ParamUnit::TableSteps)
                        .withFormatter([](float v) {
                            const int steps = juce::roundToInt(v);
                            return steps == 0 ? juce::String("Off") : (steps > 0 ? "+" : "") + juce::String(steps) + " steps";
                        })
                        .inGroup("Shifts"));
    }
    for (int i = 1; i <= ChordDevice::numShifts; ++i)
    {
        const juce::String n(i);
        s.push_back(ParameterSpec::continuous("chord.velocity" + n, "Velocity " + n, 0.0f, 2.0f, 1.0f, ParamUnit::Percent)
                        .withFormatter([](float v) { return juce::String(juce::roundToInt(v * 100.0f)) + " %"; })
                        .inGroup("Velocities"));
    }
    return s;
}
} // namespace

ChordDevice::ChordDevice() : StudioDevice(typeId, "Chord", DeviceKind::MidiEffect, makeSpecs())
{
    reset();
}

void ChordDevice::reset() noexcept
{
    for (auto& channel : held)
        for (auto& h : channel)
        {
            h.down = false;
            h.notes.fill(-1);
        }
}

void ChordDevice::process(DeviceBuffers& io, const DeviceTimeInfo&) noexcept
{
    for (const auto& e : *io.midiIn)
    {
        const int status = e.bytes[0] & 0xf0;
        const int channel = e.bytes[0] & 0x0f;
        if (isNoteOn(e))
        {
            auto& h = held[static_cast<std::size_t>(channel)][e.bytes[1] & 127];
            if (h.down) // the same key again without a release: end what it sent first
                for (auto& n : h.notes)
                    if (n >= 0)
                    {
                        io.midiOut->add(e.sampleOffset, static_cast<std::uint8_t>(0x80 | channel), static_cast<std::uint8_t>(n), 0);
                        n = -1;
                    }
            h.down = true;
            h.notes[0] = static_cast<std::int16_t>(e.bytes[1]);
            io.midiOut->add(e);
            for (int i = 0; i < numShifts; ++i)
            {
                const int shift = juce::roundToInt(value(i));
                const int note = e.bytes[1] + shift;
                if (shift == 0 || note < 0 || note > 127)
                    continue;
                const int velocity = juce::jlimit(1, 127, juce::roundToInt(e.bytes[2] * value(numShifts + i)));
                h.notes[static_cast<std::size_t>(i + 1)] = static_cast<std::int16_t>(note);
                io.midiOut->add(e.sampleOffset, e.bytes[0], static_cast<std::uint8_t>(note), static_cast<std::uint8_t>(velocity));
            }
        }
        else if (isNoteOff(e))
        {
            auto& h = held[static_cast<std::size_t>(channel)][e.bytes[1] & 127];
            if (!h.down)
            {
                io.midiOut->add(e);
                continue;
            }
            for (auto& n : h.notes)
                if (n >= 0)
                {
                    io.midiOut->add(e.sampleOffset, static_cast<std::uint8_t>(0x80 | channel), static_cast<std::uint8_t>(n), 0);
                    n = -1;
                }
            h.down = false;
        }
        else
        {
            juce::ignoreUnused(status);
            io.midiOut->add(e);
        }
    }
}

} // namespace lattice
