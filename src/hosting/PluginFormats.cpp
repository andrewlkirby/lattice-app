#include "hosting/PluginFormats.h"

#include "hosting/clap/ClapFormat.h"

namespace lattice::hosting
{

void addHostedFormats(juce::AudioPluginFormatManager& formats)
{
    formats.addFormat(std::make_unique<ClapPluginFormat>());
#if JUCE_PLUGINHOST_VST3
    formats.addFormat(std::make_unique<juce::VST3PluginFormat>());
#endif
#if JUCE_PLUGINHOST_AU && JUCE_MAC
    formats.addFormat(std::make_unique<juce::AudioUnitPluginFormat>());
#endif
#if JUCE_PLUGINHOST_LV2
    formats.addFormat(std::make_unique<juce::LV2PluginFormat>());
#endif
}

juce::StringArray hostedFormatNames()
{
    // Without making the formats: LV2's loads every bundle's description when it's made.
    juce::StringArray names{ClapPluginFormat::formatName};
#if JUCE_PLUGINHOST_VST3
    names.add("VST3");
#endif
#if JUCE_PLUGINHOST_AU && JUCE_MAC
    names.add("AudioUnit");
#endif
#if JUCE_PLUGINHOST_LV2
    names.add("LV2");
#endif
    return names;
}

} // namespace lattice::hosting
