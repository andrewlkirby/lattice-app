#include "devices/test/TestSynthDevice.h"

#include <algorithm>

namespace lattice
{
namespace
{
enum Param : std::uint8_t
{
    sawMix
};

std::vector<ParameterSpec> specs()
{
    std::vector<ParameterSpec> s;
    s.push_back(ParameterSpec::continuous("sawMix", "Saw Mix", 0.0f, 1.0f, 0.35f, ParamUnit::Percent)
                    .inGroup("Sound")
                    .withFormatter([](float v) { return juce::String(juce::roundToInt(v * 100.0f)) + " %"; }));
    return s;
}
} // namespace

TestSynthDevice::TestSynthDevice(const TrackTuning& tuning)
    : StudioDevice(typeId, "Test Synth", DeviceKind::Instrument, specs()), testSynth(tuning)
{
}

void TestSynthDevice::prepare(double sampleRate, int)
{
    testSynth.prepare(sampleRate);
}

void TestSynthDevice::process(DeviceBuffers& io, const DeviceTimeInfo&) noexcept
{
    testSynth.setSawMix(value(sawMix));
    float* left = io.channels[0];
    testSynth.render(left, io.numSamples, io.midiIn->events, io.midiIn->count);
    for (int ch = 1; ch < io.numChannels; ++ch)
        std::copy_n(left, io.numSamples, io.channels[ch]);
}

} // namespace lattice
