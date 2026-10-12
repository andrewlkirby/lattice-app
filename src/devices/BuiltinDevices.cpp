#include "devices/effects/AutoFilter.h"
#include "devices/effects/Chorus.h"
#include "devices/effects/Compressor.h"
#include "devices/effects/Delay.h"
#include "devices/effects/EqEight.h"
#include "devices/effects/Limiter.h"
#include "devices/effects/Reverb.h"
#include "devices/effects/Saturator.h"
#include "devices/effects/Utility.h"
#include "devices/framework/DeviceRegistry.h"
#include "devices/instruments/Sampler.h"
#include "devices/instruments/Synth.h"
#include "devices/midi/Arpeggiator.h"
#include "devices/midi/Chord.h"
#include "devices/test/TestSynthDevice.h"

namespace lattice
{

void registerBuiltinDevices(DeviceRegistry& registry)
{
    registry.add({SynthDevice::typeId, "Synth", "Instruments", DeviceKind::Instrument, false,
                  [](const DeviceCreateContext& c) -> std::unique_ptr<StudioDevice> {
                      jassert(c.tuning != nullptr);
                      return std::make_unique<SynthDevice>(*c.tuning);
                  }});
    registry.add({ArpeggiatorDevice::typeId, "Arpeggiator", "MIDI Effects", DeviceKind::MidiEffect, false,
                  [](const DeviceCreateContext&) -> std::unique_ptr<StudioDevice> {
                      return std::make_unique<ArpeggiatorDevice>();
                  }});
    registry.add({ChordDevice::typeId, "Chord", "MIDI Effects", DeviceKind::MidiEffect, false,
                  [](const DeviceCreateContext&) -> std::unique_ptr<StudioDevice> {
                      return std::make_unique<ChordDevice>();
                  }});
    registry.add({SamplerDevice::typeId, "Sampler", "Instruments", DeviceKind::Instrument, false,
                  [](const DeviceCreateContext& c) -> std::unique_ptr<StudioDevice> {
                      jassert(c.tuning != nullptr);
                      return std::make_unique<SamplerDevice>(*c.tuning,
                                                             c.samples != nullptr ? *c.samples : SamplePool::shared());
                  }});
    registry.add({AutoFilterDevice::typeId, "Auto Filter", "Audio Effects", DeviceKind::AudioEffect, false,
                  [](const DeviceCreateContext&) -> std::unique_ptr<StudioDevice> { return std::make_unique<AutoFilterDevice>(); }});
    registry.add({ChorusDevice::typeId, "Chorus", "Audio Effects", DeviceKind::AudioEffect, false,
                  [](const DeviceCreateContext&) -> std::unique_ptr<StudioDevice> { return std::make_unique<ChorusDevice>(); }});
    registry.add({CompressorDevice::typeId, "Compressor", "Audio Effects", DeviceKind::AudioEffect, false,
                  [](const DeviceCreateContext&) -> std::unique_ptr<StudioDevice> { return std::make_unique<CompressorDevice>(); }});
    registry.add({DelayDevice::typeId, "Delay", "Audio Effects", DeviceKind::AudioEffect, false,
                  [](const DeviceCreateContext&) -> std::unique_ptr<StudioDevice> { return std::make_unique<DelayDevice>(); }});
    registry.add({EqEightDevice::typeId, "EQ Eight", "Audio Effects", DeviceKind::AudioEffect, false,
                  [](const DeviceCreateContext&) -> std::unique_ptr<StudioDevice> { return std::make_unique<EqEightDevice>(); }});
    registry.add({LimiterDevice::typeId, "Limiter", "Audio Effects", DeviceKind::AudioEffect, false,
                  [](const DeviceCreateContext&) -> std::unique_ptr<StudioDevice> { return std::make_unique<LimiterDevice>(); }});
    registry.add({ReverbDevice::typeId, "Reverb", "Audio Effects", DeviceKind::AudioEffect, false,
                  [](const DeviceCreateContext&) -> std::unique_ptr<StudioDevice> { return std::make_unique<ReverbDevice>(); }});
    registry.add({SaturatorDevice::typeId, "Saturator", "Audio Effects", DeviceKind::AudioEffect, false,
                  [](const DeviceCreateContext&) -> std::unique_ptr<StudioDevice> { return std::make_unique<SaturatorDevice>(); }});
    registry.add({UtilityDevice::typeId, "Utility", "Audio Effects", DeviceKind::AudioEffect, false,
                  [](const DeviceCreateContext&) -> std::unique_ptr<StudioDevice> { return std::make_unique<UtilityDevice>(); }});
    registry.add({TestSynthDevice::typeId, "Test Synth", "Instruments", DeviceKind::Instrument, true,
                  [](const DeviceCreateContext& c) -> std::unique_ptr<StudioDevice> {
                      jassert(c.tuning != nullptr);
                      return std::make_unique<TestSynthDevice>(*c.tuning);
                  }});
}

} // namespace lattice
