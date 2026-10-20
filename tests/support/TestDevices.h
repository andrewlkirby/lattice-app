#pragma once

#include "devices/framework/DeviceRegistry.h"

#include <array>
#include <atomic>
#include <cmath>

namespace lattice::test
{

/** Audio effect: multiplies by "gain" (-1 flips the polarity, the worst case for a switch). */
class TestGainDevice final : public StudioDevice
{
public:
    static constexpr const char* typeId = "test.gain";
    TestGainDevice()
        : StudioDevice(typeId, "Test Gain", DeviceKind::AudioEffect,
                       {ParameterSpec::continuous("gain", "Gain", -1.0f, 2.0f, 1.0f).inGroup("Main")})
    {
    }

protected:
    void prepare(double, int) override {}
    void process(DeviceBuffers& io, const DeviceTimeInfo&) noexcept override
    {
        const float g = value(0);
        for (int ch = 0; ch < io.numChannels; ++ch)
            for (int i = 0; i < io.numSamples; ++i)
                io.channels[ch][i] *= g;
    }
};

/** Audio effect: a pure delay of `latency` samples, reported as its latency. */
class TestLatencyDevice final : public StudioDevice
{
public:
    static constexpr const char* typeId = "test.latency";
    static constexpr int latency = 100;
    TestLatencyDevice() : StudioDevice(typeId, "Test Latency", DeviceKind::AudioEffect, {}) {}

protected:
    void prepare(double, int) override
    {
        setLatency(latency);
        for (auto& r : rings)
            r.fill(0.0f);
        pos = 0;
    }
    void process(DeviceBuffers& io, const DeviceTimeInfo&) noexcept override
    {
        int p = pos;
        for (int i = 0; i < io.numSamples; ++i)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                const float in = io.channels[ch][i];
                io.channels[ch][i] = rings[static_cast<std::size_t>(ch)][static_cast<std::size_t>(p)];
                rings[static_cast<std::size_t>(ch)][static_cast<std::size_t>(p)] = in;
            }
            p = (p + 1) % latency;
        }
        pos = p;
    }
    void reset() noexcept override
    {
        for (auto& r : rings)
            r.fill(0.0f);
    }

private:
    std::array<std::array<float, latency>, 2> rings{};
    int pos = 0;
};

/** MIDI effect: transposes notes by "amount". */
class TestTransposeDevice final : public StudioDevice
{
public:
    static constexpr const char* typeId = "test.transpose";
    TestTransposeDevice()
        : StudioDevice(typeId, "Test Transpose", DeviceKind::MidiEffect,
                       {ParameterSpec::integer("amount", "Amount", -24, 24, 12).inGroup("Main")})
    {
    }

protected:
    void prepare(double, int) override {}
    void process(DeviceBuffers& io, const DeviceTimeInfo&) noexcept override
    {
        const int amount = juce::roundToInt(value(0));
        for (auto e : *io.midiIn)
        {
            if (isNoteOn(e) || isNoteOff(e))
                e.bytes[1] = static_cast<std::uint8_t>(juce::jlimit(0, 127, e.bytes[1] + amount));
            io.midiOut->add(e);
        }
    }
};

/** Instrument: a steady 100 Hz sine at 0.5 that ignores MIDI, and records the notes it is sent. */
class TestSineDevice final : public StudioDevice
{
public:
    static constexpr const char* typeId = "test.sine";
    TestSineDevice() : StudioDevice(typeId, "Test Sine", DeviceKind::Instrument, {}) {}

    std::array<std::atomic<int>, 128> held{}; // note-ons minus note-offs per note
    std::atomic<int> releases{0};

    void releaseAllNotes() noexcept override
    {
        releases.fetch_add(1);
        for (auto& h : held)
            h.store(0);
    }

protected:
    void prepare(double sr, int) override { increment = 100.0 / sr; }
    void process(DeviceBuffers& io, const DeviceTimeInfo&) noexcept override
    {
        for (const auto& e : *io.midiIn)
        {
            if (isNoteOn(e))
                held[e.bytes[1]].fetch_add(1);
            else if (isNoteOff(e) && held[e.bytes[1]].load() > 0)
                held[e.bytes[1]].fetch_sub(1);
        }
        for (int i = 0; i < io.numSamples; ++i)
        {
            const auto v = static_cast<float>(0.5 * std::sin(phase * 2.0 * juce::MathConstants<double>::pi));
            phase += increment;
            if (phase >= 1.0)
                phase -= 1.0;
            for (int ch = 0; ch < io.numChannels; ++ch)
                io.channels[ch][i] = v;
        }
    }

private:
    double phase = 0.0;
    double increment = 0.0;
};

/** Instrument: records the note-ons it gets (note and velocity) and outputs a steady level of
    0.25 per held note on both channels (so mixing and gains are easy to check). Choking or
    releasing ends every note at once. */
class TestRecorderDevice final : public StudioDevice
{
public:
    static constexpr const char* typeId = "test.recorder";
    TestRecorderDevice() : StudioDevice(typeId, "Test Recorder", DeviceKind::Instrument, {}) {}

    struct NoteOn
    {
        int note = 0;
        int velocity = 0;
    };
    std::array<NoteOn, 256> received{};
    std::atomic<int> count{0};
    std::array<int, 128> held{};
    std::atomic<int> chokes{0};

    void releaseAllNotes() noexcept override { held.fill(0); }
    void choke() noexcept override
    {
        chokes.fetch_add(1);
        held.fill(0);
    }

protected:
    void prepare(double, int) override { held.fill(0); }
    void process(DeviceBuffers& io, const DeviceTimeInfo&) noexcept override
    {
        int pos = 0;
        const auto fill = [&](int to) {
            int sounding = 0;
            for (const int h : held)
                sounding += h > 0 ? 1 : 0;
            for (int ch = 0; ch < io.numChannels; ++ch)
                for (int i = pos; i < to; ++i)
                    io.channels[ch][i] = 0.25f * static_cast<float>(sounding);
            pos = to;
        };
        for (const auto& e : *io.midiIn)
        {
            fill(e.sampleOffset);
            if (isNoteOn(e))
            {
                ++held[e.bytes[1]];
                const int n = count.load();
                if (n < static_cast<int>(received.size()))
                {
                    received[static_cast<std::size_t>(n)] = {e.bytes[1], e.bytes[2]};
                    count.store(n + 1);
                }
            }
            else if (isNoteOff(e) && held[e.bytes[1]] > 0)
            {
                --held[e.bytes[1]];
            }
        }
        fill(io.numSamples);
    }
};

/** The built-in devices plus the test devices above. */
inline const DeviceRegistry& testRegistry()
{
    static const DeviceRegistry registry = [] {
        DeviceRegistry r;
        registerBuiltinDevices(r);
        r.add({TestGainDevice::typeId, "Test Gain", "Test", DeviceKind::AudioEffect, true,
               [](const DeviceCreateContext&) { return std::make_unique<TestGainDevice>(); }});
        r.add({TestLatencyDevice::typeId, "Test Latency", "Test", DeviceKind::AudioEffect, true,
               [](const DeviceCreateContext&) { return std::make_unique<TestLatencyDevice>(); }});
        r.add({TestTransposeDevice::typeId, "Test Transpose", "Test", DeviceKind::MidiEffect, true,
               [](const DeviceCreateContext&) { return std::make_unique<TestTransposeDevice>(); }});
        r.add({TestSineDevice::typeId, "Test Sine", "Test", DeviceKind::Instrument, true,
               [](const DeviceCreateContext&) { return std::make_unique<TestSineDevice>(); }});
        r.add({TestRecorderDevice::typeId, "Test Recorder", "Test", DeviceKind::Instrument, true,
               [](const DeviceCreateContext&) { return std::make_unique<TestRecorderDevice>(); }});
        return r;
    }();
    return registry;
}

} // namespace lattice::test
