#include "devices/framework/DeviceRegistry.h"
#include "devices/test/TestSynthDevice.h"

namespace lattice
{

void registerBuiltinDevices(DeviceRegistry& registry)
{
    registry.add({TestSynthDevice::typeId, "Test Synth", "Instruments", DeviceKind::Instrument, true,
                  [](const DeviceCreateContext& c) -> std::unique_ptr<StudioDevice> {
                      jassert(c.tuning != nullptr);
                      return std::make_unique<TestSynthDevice>(*c.tuning);
                  }});
}

} // namespace lattice
