#pragma once

#include "core/RealtimeCheck.h"
#include "devices/framework/StudioDevice.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace lattice::test
{

/** Runs one device directly, without an engine (docs/04-m3-devices.md, "Tests"): MIDI events at
    absolute samples, any block size, everything on a simulated audio thread so allocations there
    are counted. Records stereo output (and, for MIDI effects, the output events). */
class DeviceHarness
{
public:
    static constexpr double sampleRate = 48000.0;

    struct Event
    {
        std::int64_t sample = 0;
        std::uint8_t status = 0;
        std::uint8_t data1 = 0;
        std::uint8_t data2 = 0;
    };

    explicit DeviceHarness(StudioDevice& d, int maxBlock = 512) : device(d), blockLimit(maxBlock)
    {
        device.prepareDevice(sampleRate, maxBlock);
        in.assign(4096, TimedMidi{});
        out.assign(4096, TimedMidi{});
        left.assign(static_cast<std::size_t>(maxBlock), 0.0f);
        right.assign(static_cast<std::size_t>(maxBlock), 0.0f);
    }

    void noteOn(std::int64_t at, int note, int velocity = 100, int channel = 0)
    {
        add({at, static_cast<std::uint8_t>(0x90 | channel), static_cast<std::uint8_t>(note),
             static_cast<std::uint8_t>(velocity)});
    }
    void noteOff(std::int64_t at, int note, int channel = 0)
    {
        add({at, static_cast<std::uint8_t>(0x80 | channel), static_cast<std::uint8_t>(note), 0});
    }
    void controller(std::int64_t at, int cc, int value)
    {
        add({at, 0xb0, static_cast<std::uint8_t>(cc), static_cast<std::uint8_t>(value)});
    }
    void add(const Event& e)
    {
        events.push_back(e);
        std::stable_sort(events.begin(), events.end(), [](const Event& a, const Event& b) { return a.sample < b.sample; });
    }

    /** Renders up to the absolute sample `target`, in blocks of `blockSize` (0 = random sizes). */
    void renderTo(std::int64_t target, int blockSize = 64)
    {
        while (position < target)
        {
            int n = blockSize > 0 ? blockSize : 1 + static_cast<int>(nextRandom() % static_cast<std::uint32_t>(blockLimit));
            n = static_cast<int>(std::min<std::int64_t>(std::min(n, blockLimit), target - position));
            MidiBlock midiIn{in.data(), static_cast<int>(in.size()), 0, 0};
            MidiBlock midiOut{out.data(), static_cast<int>(out.size()), 0, 0};
            while (nextEvent < events.size() && events[nextEvent].sample < position + n)
            {
                const auto& e = events[nextEvent++];
                midiIn.add(static_cast<int>(std::max<std::int64_t>(0, e.sample - position)), e.status, e.data1, e.data2);
            }
            float* channels[2] = {left.data(), right.data()};
            if (inputSignal)
            {
                for (int i = 0; i < n; ++i)
                {
                    const auto [l, r] = inputSignal(position + i);
                    left[static_cast<std::size_t>(i)] = l;
                    right[static_cast<std::size_t>(i)] = r;
                }
            }
            else
            {
                std::fill_n(left.begin(), n, 0.0f);
                std::fill_n(right.begin(), n, 0.0f);
            }
            DeviceBuffers io;
            io.channels = channels;
            io.numChannels = 2;
            io.numSamples = n;
            io.midiIn = &midiIn;
            io.midiOut = &midiOut;
            io.sidechain = sidechain;
            DeviceTimeInfo time;
            time.sampleRate = sampleRate;
            time.tempo = tempo;
            time.samplesPerBeat = sampleRate * 60.0 / tempo;
            time.playing = playing;
            time.beatAtStart = static_cast<double>(position) / time.samplesPerBeat;
            time.blockStartSample = position;
            {
                const rt::ScopedAudioThread audioThread;
                device.processInChain(io, time);
            }
            audioLeft.insert(audioLeft.end(), left.begin(), left.begin() + n);
            audioRight.insert(audioRight.end(), right.begin(), right.begin() + n);
            for (const auto& e : midiOut)
                outputEvents.push_back({position + e.sampleOffset, e.bytes[0], e.bytes[1], e.bytes[2]});
            position += n;
        }
    }

    std::int64_t now() const noexcept { return position; }

    /** Samples [from, to) of the left channel. */
    std::vector<float> slice(std::int64_t from, std::int64_t to) const
    {
        return {audioLeft.begin() + from, audioLeft.begin() + to};
    }

    StudioDevice& device;
    std::vector<Event> events;
    std::vector<Event> outputEvents;
    std::vector<float> audioLeft;
    std::vector<float> audioRight;
    std::function<std::pair<float, float>(std::int64_t sample)> inputSignal; // effects: the input
    const float* const* sidechain = nullptr;
    double tempo = 120.0;
    bool playing = true;

private:
    std::uint32_t nextRandom()
    {
        rng = rng * 1664525u + 1013904223u;
        return rng >> 8;
    }

    int blockLimit;
    std::vector<TimedMidi> in;
    std::vector<TimedMidi> out;
    std::vector<float> left;
    std::vector<float> right;
    std::size_t nextEvent = 0;
    std::int64_t position = 0;
    std::uint32_t rng = 99;
};

/** Sets a device parameter by ID (its base value, as the document would). */
inline void setParam(StudioDevice& d, const char* id, float value)
{
    auto* p = d.findParameter(id);
    REQUIRE(p != nullptr);
    p->setBase(value);
}

inline float largestStep(const std::vector<float>& signal, std::size_t from = 1, std::size_t to = 0)
{
    if (to == 0 || to > signal.size())
        to = signal.size();
    float largest = 0.0f;
    for (auto i = std::max<std::size_t>(from, 1); i < to; ++i)
        largest = std::max(largest, std::abs(signal[i] - signal[i - 1]));
    return largest;
}

inline float peakOf(const std::vector<float>& signal, std::size_t from = 0, std::size_t to = 0)
{
    if (to == 0 || to > signal.size())
        to = signal.size();
    float p = 0.0f;
    for (auto i = from; i < to; ++i)
        p = std::max(p, std::abs(signal[i]));
    return p;
}

/** Average frequency from rising zero crossings within [from, to) (linear interpolation). */
inline double zeroCrossingHz(const std::vector<float>& s, std::size_t from, std::size_t to,
                             double sr = DeviceHarness::sampleRate)
{
    double first = -1.0;
    double last = -1.0;
    int count = 0;
    for (auto i = from + 1; i < to; ++i)
    {
        if (s[i - 1] < 0.0f && s[i] >= 0.0f)
        {
            const double t = static_cast<double>(i - 1) + s[i - 1] / (s[i - 1] - s[i]);
            if (first < 0.0)
                first = t;
            else
                ++count;
            last = t;
        }
    }
    return count > 0 ? count * sr / (last - first) : 0.0;
}

} // namespace lattice::test
