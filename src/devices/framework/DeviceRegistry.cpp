#include "devices/framework/DeviceRegistry.h"

namespace lattice
{

const DeviceRegistry& DeviceRegistry::builtin()
{
    static const DeviceRegistry registry = [] {
        DeviceRegistry r;
        registerBuiltinDevices(r);
        return r;
    }();
    return registry;
}

void DeviceRegistry::add(DeviceTypeInfo type)
{
    jassert(find(type.id) == nullptr);
    all.push_back(std::move(type));
}

const DeviceTypeInfo* DeviceRegistry::find(const juce::String& typeId) const noexcept
{
    for (const auto& t : all)
        if (t.id == typeId)
            return &t;
    return nullptr;
}

std::unique_ptr<StudioDevice> DeviceRegistry::create(const juce::String& typeId,
                                                     const DeviceCreateContext& context) const
{
    if (const auto* t = find(typeId); t != nullptr && t->create)
        if (auto device = t->create(context))
            return device;
    return std::make_unique<MissingDevice>(typeId);
}

DeviceKind DeviceRegistry::kindOf(const juce::String& typeId) const noexcept
{
    const auto* t = find(typeId);
    return t != nullptr ? t->kind : DeviceKind::AudioEffect;
}

MissingDevice::MissingDevice(const juce::String& missingTypeId)
    : StudioDevice(missingTypeId, "Missing device (" + missingTypeId + ")", DeviceKind::AudioEffect, {})
{
}

} // namespace lattice
