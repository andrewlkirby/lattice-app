#pragma once

#include "devices/framework/StudioDevice.h"

#include <juce_data_structures/juce_data_structures.h>

#include <memory>

namespace lattice
{

/** Makes the devices of types that DeviceRegistry can't make on the spot: plugins
    (docs/07-m6-plugins-external.md, 6b), which load in the background. The snapshot build asks it
    for those types instead of the registry. Message thread. */
struct DeviceSource
{
    virtual ~DeviceSource() = default;

    /** Whether it makes devices of this type. */
    virtual bool makes(const juce::String& typeId) const = 0;

    /** The device for a node: the real one if it's ready, a stand-in while it loads or if it can't
        be loaded. Never null. */
    virtual std::unique_ptr<StudioDevice> create(const juce::ValueTree& deviceNode, const DeviceCreateContext& context) = 0;

    /** Whether the instance in a chain should be made again: a stand-in whose device is now ready
        (or has failed), or a device that asked to be restarted. */
    virtual bool isStale(const StudioDevice& device, const juce::ValueTree& deviceNode) const = 0;
};

} // namespace lattice
