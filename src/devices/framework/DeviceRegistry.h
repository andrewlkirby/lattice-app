#pragma once

#include "devices/framework/StudioDevice.h"

#include <functional>
#include <memory>
#include <vector>

namespace lattice
{

/** One kind of device that can be put on a track. */
struct DeviceTypeInfo
{
    juce::String id;       // stable forever: saved sets and presets refer to it ("lattice.synth")
    juce::String name;     // shown in the device list ("Synth")
    juce::String category; // "Instruments", "MIDI Effects", "Audio Effects"
    DeviceKind kind = DeviceKind::AudioEffect;
    bool hidden = false; // loadable, but not offered in the device list (test devices)
    std::function<std::unique_ptr<StudioDevice>(const DeviceCreateContext&)> create;
};

/** The device types the app knows (docs/04-m3-devices.md). Message thread. */
class DeviceRegistry
{
public:
    /** Every built-in device. */
    static const DeviceRegistry& builtin();

    void add(DeviceTypeInfo type);
    const DeviceTypeInfo* find(const juce::String& typeId) const noexcept;
    const std::vector<DeviceTypeInfo>& types() const noexcept { return all; }

    /** A new device of that type. An unknown type (a set from a newer version, a removed device)
        gives a stand-in that passes audio through and keeps the saved state, so saving the set
        again loses nothing. */
    std::unique_ptr<StudioDevice> create(const juce::String& typeId, const DeviceCreateContext& context) const;

    /** The kind of a type; AudioEffect for unknown types. */
    DeviceKind kindOf(const juce::String& typeId) const noexcept;

private:
    std::vector<DeviceTypeInfo> all;
};

/** Adds every built-in device type (devices/BuiltinDevices.cpp). */
void registerBuiltinDevices(DeviceRegistry& registry);

/** The pass-through stand-in for a device type that isn't available. */
class MissingDevice final : public StudioDevice
{
public:
    explicit MissingDevice(const juce::String& missingTypeId);

protected:
    void prepare(double, int) override {}
    void process(DeviceBuffers&, const DeviceTimeInfo&) noexcept override {}
};

} // namespace lattice
