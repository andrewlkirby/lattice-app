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
    registry.add({TestSynthDevice::typeId, "Test Synth", "Instruments", DeviceKind::Instrument, true,
                  [](const DeviceCreateContext& c) -> std::unique_ptr<StudioDevice> {
                      jassert(c.tuning != nullptr);
                      return std::make_unique<TestSynthDevice>(*c.tuning);
                  }});
}

} // namespace lattice
