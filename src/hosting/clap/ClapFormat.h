#pragma once

#include "hosting/scan/ScanProtocol.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <vector>

namespace lattice::hosting
{

/** CLAP plugins (docs/07-m6-plugins-external.md, 6a). JUCE 8 hosts no CLAP, so this is Lattice's
    own format, behind JUCE's interface, from the CLAP headers (MIT). A .clap file can hold several
    plugins; each is described from its descriptor, and briefly instantiated for its audio and
    note ports (which the descriptor doesn't give), as JUCE's VST3 scan does.

    Scanning loads the plugin's code, so the app scans only in LatticePluginScanner. Loading a CLAP
    plugin as a device comes in 6b: until then createPluginInstance() reports that it can't.
    Message thread (CLAP's main thread). */
class ClapPluginFormat final : public juce::AudioPluginFormat
{
public:
    static constexpr const char* formatName = "CLAP";

    /** Every plugin in a .clap file. Empty, with `error` set, if it can't be read. */
    static std::vector<scan::ScannedPlugin> scanFile(const juce::String& path, juce::String& error);

    /** A CLAP id's number for juce::PluginDescription (whose identifier needs one): stable across
        runs and machines. */
    static int uniqueIdOf(const juce::String& clapId);

    juce::String getName() const override { return formatName; }
    void findAllTypesForFile(juce::OwnedArray<juce::PluginDescription>& results, const juce::String& fileOrIdentifier) override;
    bool fileMightContainThisPluginType(const juce::String& fileOrIdentifier) override;
    juce::String getNameOfPluginFromIdentifier(const juce::String& fileOrIdentifier) override;
    bool pluginNeedsRescanning(const juce::PluginDescription& description) override;
    bool doesPluginStillExist(const juce::PluginDescription& description) override;
    bool canScanForPlugins() const override { return true; }
    bool isTrivialToScan() const override { return false; }
    juce::StringArray searchPathsForPlugins(const juce::FileSearchPath& directories, bool recursive, bool allowAsync) override;
    juce::FileSearchPath getDefaultLocationsToSearch() override;
    bool requiresUnblockedMessageThreadDuringCreation(const juce::PluginDescription&) const override { return false; }

protected:
    void createPluginInstance(const juce::PluginDescription&, double initialSampleRate, int initialBufferSize,
                              PluginCreationCallback callback) override;
};

} // namespace lattice::hosting
