// Device benchmarks (docs/04-m3-devices.md, "Tests" 11): each device at 48 kHz with 64- and
// 256-sample blocks, reported as the share of one core it needs in real time.
//
//   lattice_bench [--seconds N] [--filter name] [--simd sse2|avx2]
//
// --simd runs the kernels of that SIMD level (at most the CPU's; devices/dsp/Simd.h).

#include "devices/dsp/Simd.h"
#include "devices/effects/AutoFilter.h"
#include "devices/effects/Chorus.h"
#include "devices/effects/Compressor.h"
#include "devices/effects/Delay.h"
#include "devices/effects/EqEight.h"
#include "devices/effects/Limiter.h"
#include "devices/effects/Reverb.h"
#include "devices/effects/Saturator.h"
#include "devices/effects/Utility.h"
#include "devices/instruments/Sampler.h"
#include "devices/instruments/Synth.h"
#include "tuning/Tet12.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using namespace lattice;

namespace
{
constexpr double sampleRate = 48000.0;

struct Case
{
    std::string name;
    double budgetPercent;
    std::function<std::unique_ptr<StudioDevice>()> make;
    std::function<void(StudioDevice&)> configure;
    int notes = 0; // held notes, spread over the keyboard
};

void setParam(StudioDevice& d, const char* id, float value)
{
    if (auto* p = d.findParameter(id))
        p->setBase(value);
    else
        std::fprintf(stderr, "unknown parameter %s\n", id);
}

/** Percent of one core: processing time / audio time. */
double run(const Case& c, int block, double seconds)
{
    auto device = c.make();
    c.configure(*device);
    device->prepareDevice(sampleRate, block);

    std::vector<TimedMidi> eventStorage(4096);
    std::vector<TimedMidi> outStorage(4096);
    std::vector<float> left(static_cast<std::size_t>(block)), right(static_cast<std::size_t>(block));
    float* channels[2] = {left.data(), right.data()};
    DeviceTimeInfo time;
    time.sampleRate = sampleRate;
    time.playing = true;

    const auto blocks = static_cast<std::int64_t>(seconds * sampleRate / block);
    const std::int64_t warmup = static_cast<std::int64_t>(0.5 * sampleRate / block);
    double busy = 0.0;
    for (std::int64_t b = 0; b < warmup + blocks; ++b)
    {
        MidiBlock in{eventStorage.data(), 4096, 0, 0};
        MidiBlock out{outStorage.data(), 4096, 0, 0};
        if (b == 0)
            for (int n = 0; n < c.notes; ++n)
                in.add(0, 0x90, static_cast<std::uint8_t>(c.notes > 16 ? 48 + n : 36 + n * 3), 100);
        for (int i = 0; i < block; ++i)
            left[static_cast<std::size_t>(i)] = right[static_cast<std::size_t>(i)] =
                0.1f * static_cast<float>((i * 7919 + b * 31) % 200 - 100) / 100.0f;
        DeviceBuffers io;
        io.channels = channels;
        io.numSamples = block;
        io.midiIn = &in;
        io.midiOut = &out;
        time.beatAtStart = static_cast<double>(b * block) / time.samplesPerBeat;
        const auto t0 = std::chrono::steady_clock::now();
        device->processInChain(io, time);
        const auto t1 = std::chrono::steady_clock::now();
        if (b >= warmup)
            busy += std::chrono::duration<double>(t1 - t0).count();
    }
    return 100.0 * busy / (static_cast<double>(blocks * block) / sampleRate);
}
} // namespace

int main(int argc, char* argv[])
{
    double seconds = 5.0;
    std::string filter;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            seconds = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc)
            filter = argv[++i];
        else if (std::strcmp(argv[i], "--simd") == 0 && i + 1 < argc)
        {
            const std::string level = argv[++i];
            dsp::setSimdLevel(level == "sse2" ? dsp::SimdLevel::Sse2 : dsp::SimdLevel::Avx2);
        }
    }

    static TrackTuning tuning(Tet12::make());
    tuning.setSampleRate(sampleRate);
    const auto synth = [] { return std::make_unique<SynthDevice>(tuning); };

    // A 10 s stereo sample of decaying noise bursts, at 44.1 kHz (so every note resamples).
    static const SamplePtr drums = [] {
        std::vector<std::vector<float>> ch(2, std::vector<float>(441000));
        std::uint32_t seed = 1;
        for (std::size_t i = 0; i < ch[0].size(); ++i)
        {
            seed = seed * 1664525u + 1013904223u;
            const float env = std::exp(-static_cast<float>(i % 22050) / 4000.0f);
            ch[0][i] = env * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f);
            ch[1][i] = 0.8f * ch[0][i];
        }
        return SampleData::fromChannels(ch, 44100.0, "bench");
    }();
    const auto sampler = [] {
        auto s = std::make_unique<SamplerDevice>(tuning, SamplePool::shared());
        s->setSample(drums);
        return s;
    };

    std::vector<Case> cases{
        {"Synth 16 voices x 2 osc, unison 1", 2.0, synth, [](StudioDevice&) {}, 16},
        {"Synth 16 voices x 2 osc, unison 4", 2.0, synth,
         [](StudioDevice& d) { setParam(d, "unison.voices", 4.0f); }, 16},
        {"Synth 16 voices x 2 osc, unison 4 mono", 2.0, synth,
         [](StudioDevice& d) {
             setParam(d, "unison.voices", 4.0f);
             setParam(d, "unison.spread", 0.0f);
         },
         16},
        {"Synth 16 voices x 1 osc, 12 dB", 2.0, synth,
         [](StudioDevice& d) {
             setParam(d, "osc2.level", -70.0f);
             setParam(d, "filter.type", 0.0f);
         },
         16},
        {"Sampler 32 voices, sinc", 1.5, sampler,
         [](StudioDevice& d) { setParam(d, "sample.root", 261.6256f); }, 32},
        {"Sampler 32 voices, draft", 1.5, sampler,
         [](StudioDevice& d) { setParam(d, "sample.quality", 1.0f); }, 32},
        {"Reverb, High", 1.0, [] { return std::make_unique<ReverbDevice>(); }, [](StudioDevice&) {}, 0},
        {"Reverb, Ultra", 2.0, [] { return std::make_unique<ReverbDevice>(); },
         [](StudioDevice& d) { setParam(d, "reverb.quality", 2.0f); }, 0},
        {"Reverb, Eco", 0.5, [] { return std::make_unique<ReverbDevice>(); },
         [](StudioDevice& d) { setParam(d, "reverb.quality", 0.0f); }, 0},
        {"EQ Eight, 8 bands", 0.5, [] { return std::make_unique<EqEightDevice>(); },
         [](StudioDevice& d) {
             for (int b = 1; b <= 8; ++b)
             {
                 setParam(d, ("band" + juce::String(b) + ".on").toRawUTF8(), 1.0f);
                 setParam(d, ("band" + juce::String(b) + ".gain").toRawUTF8(), 3.0f);
             }
             setParam(d, "band1.shape", 0.0f); // a 48 dB cut too
         },
         0},
        {"Compressor", 0.5, [] { return std::make_unique<CompressorDevice>(); }, [](StudioDevice&) {}, 0},
        {"Delay", 0.5, [] { return std::make_unique<DelayDevice>(); },
         [](StudioDevice& d) { setParam(d, "delay.modDepth", 2.0f); }, 0},
        {"Chorus", 0.5, [] { return std::make_unique<ChorusDevice>(); }, [](StudioDevice&) {}, 0},
        {"Auto Filter", 0.5, [] { return std::make_unique<AutoFilterDevice>(); }, [](StudioDevice&) {}, 0},
        {"Saturator 2x", 0.5, [] { return std::make_unique<SaturatorDevice>(); }, [](StudioDevice&) {}, 0},
        {"Saturator 4x", 0.5, [] { return std::make_unique<SaturatorDevice>(); },
         [](StudioDevice& d) { setParam(d, "sat.oversampling", 1.0f); }, 0},
        {"Limiter", 0.5, [] { return std::make_unique<LimiterDevice>(); }, [](StudioDevice&) {}, 0},
        {"Utility", 0.5, [] { return std::make_unique<UtilityDevice>(); }, [](StudioDevice&) {}, 0},
        {"Synth 16 voices, ladder", 2.0, synth,
         [](StudioDevice& d) {
             setParam(d, "filter.type", static_cast<float>(SynthDevice::FilterType::Ladder));
         },
         16},
    };

    std::printf("SIMD: %s (CPU: %s)\n", dsp::simdLevelName(dsp::simdLevel()), dsp::simdLevelName(dsp::cpuSimdLevel()));
    std::printf("%-40s %10s %10s %8s\n", "device", "64 smp %", "256 smp %", "budget");
    bool allWithin = true;
    for (const auto& c : cases)
    {
        if (!filter.empty() && c.name.find(filter) == std::string::npos)
            continue;
        const double p64 = run(c, 64, seconds);
        const double p256 = run(c, 256, seconds);
        const bool within = p64 <= c.budgetPercent && p256 <= c.budgetPercent;
        allWithin = allWithin && within;
        std::printf("%-40s %10.3f %10.3f %7.2f%s\n", c.name.c_str(), p64, p256, c.budgetPercent,
                    within ? "" : "  OVER");
    }
    return allWithin ? 0 : 1;
}
