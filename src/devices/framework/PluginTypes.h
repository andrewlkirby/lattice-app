#pragma once

#include "devices/framework/StudioDevice.h"

namespace lattice
{

/** The device types a plugin goes on a track as (docs/07-m6-plugins-external.md, 6b). The type
    says only what it does in a chain, so a chain's rules (MIDI effects, then an instrument, then
    audio effects) apply to plugins as to everything else; which plugin it is, and its state, are in
    the device's STATE:

    - PLUGIN: the plugin's description (juce::PluginDescription's XML: format, file, ID, name...);
    - data: the plugin's own state, base64;
    - configured: the IDs of the parameters shown on the device and on controllers, in order;
    - mpe: whether it gets MPE (-1 or absent: as its scan said).

    The device node's name is the plugin's (so it reads as "Surge XT" everywhere), and
    pluginWindow, outside STATE, where its window was. */
namespace pluginTypes
{
inline constexpr const char* instrument = "plugin.instrument";
inline constexpr const char* audioEffect = "plugin.effect";
inline constexpr const char* midiEffect = "plugin.midi";

inline bool isPlugin(const juce::String& typeId)
{
    return typeId == instrument || typeId == audioEffect || typeId == midiEffect;
}

inline DeviceKind kindOf(const juce::String& typeId)
{
    return typeId == instrument ? DeviceKind::Instrument : typeId == midiEffect ? DeviceKind::MidiEffect : DeviceKind::AudioEffect;
}

inline const juce::Identifier description{"PLUGIN"};
inline const juce::Identifier data{"data"};
inline const juce::Identifier configured{"configured"};
inline const juce::Identifier mpe{"mpe"};
inline const juce::Identifier window{"pluginWindow"};
} // namespace pluginTypes

/** A plugin device while its plugin loads, or when it can't be loaded (not installed, failed to
    start): an instrument is silent, an effect passes audio through, a MIDI effect passes MIDI
    through. It has no parameters; the document keeps the plugin's state and values untouched, so
    saving loses nothing and the plugin comes back as it was when it's there again. */
class PluginStandIn final : public StudioDevice
{
public:
    enum class Why
    {
        loading,
        missing, // not installed (or no plugin host: tests, tools)
        failed   // installed, but it wouldn't load
    };

    PluginStandIn(const juce::String& typeId, Why why, juce::String message = {})
        : StudioDevice(typeId, "Plugin", pluginTypes::kindOf(typeId), {}), reason(why), text(std::move(message))
    {
    }

    Why why() const noexcept { return reason; }
    const juce::String& message() const noexcept { return text; }

protected:
    void prepare(double, int) override {}
    void process(DeviceBuffers& io, const DeviceTimeInfo&) noexcept override
    {
        if (kind() == DeviceKind::MidiEffect && io.midiIn != nullptr && io.midiOut != nullptr)
            for (const auto& e : *io.midiIn)
                io.midiOut->add(e);
        else if (kind() == DeviceKind::Instrument)
            for (int ch = 0; ch < io.numChannels; ++ch)
                std::fill_n(io.channels[ch], io.numSamples, 0.0f);
    }

private:
    Why reason;
    juce::String text;
};

} // namespace lattice
