#include "devices/racks/Rack.h"
#include "ui/device/DevicePanels.h"

#include <map>

namespace lattice::ui::panels
{

Factory factoryFor(const juce::String& typeId)
{
    static const std::map<juce::String, Factory> factories{
        {"lattice.synth", &synth},
        {"lattice.sampler", &sampler},
        {RackDevice::instrumentRackId, &rack},
        {RackDevice::effectRackId, &rack},
        {RackDevice::drumRackId, &rack},
        {"lattice.eq8", &eqEight},
        {"lattice.compressor", &compressor},
        {"lattice.reverb", &reverb},
        {"lattice.delay", &delay},
        {"lattice.autofilter", &autoFilter},
        {"lattice.saturator", &saturator},
        {"lattice.chorus", &chorus},
        {"lattice.utility", &utility},
        {"lattice.limiter", &limiter},
        {"lattice.arpeggiator", &arpeggiator},
        {"lattice.chord", &chord},
    };
    const auto it = factories.find(typeId);
    return it != factories.end() ? it->second : nullptr;
}

} // namespace lattice::ui::panels
