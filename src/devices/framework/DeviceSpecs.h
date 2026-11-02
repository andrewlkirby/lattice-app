#pragma once

#include "devices/framework/DeviceRegistry.h"
#include "tuning/TrackTuning.h"

#include <map>
#include <memory>

namespace lattice
{

/** Parameter specs and groups for device types, from one spec-only instance per type (never
    prepared, never processed). The live instances belong to the engine. Editors read from here,
    so they describe every device the same way without touching the audio thread. Message thread. */
class DeviceSpecs
{
public:
    explicit DeviceSpecs(const DeviceRegistry& r) : registry(r) {}

    /** The type's spec-only instance (a pass-through stand-in for an unknown type). */
    const StudioDevice* forType(const juce::String& typeId)
    {
        auto& entry = cache[typeId];
        if (entry == nullptr)
        {
            DeviceCreateContext context;
            context.tuning = &tuning;
            entry = registry.create(typeId, context);
        }
        return entry.get();
    }

    /** A new spec-only instance of its own (an editor that sets its values to draw curves). */
    std::unique_ptr<StudioDevice> create(const juce::String& typeId) const
    {
        DeviceCreateContext context;
        context.tuning = &tuning;
        return registry.create(typeId, context);
    }

    const DeviceRegistry& devices() const noexcept { return registry; }

private:
    const DeviceRegistry& registry;
    TrackTuning tuning;
    std::map<juce::String, std::unique_ptr<StudioDevice>> cache;
};

} // namespace lattice
