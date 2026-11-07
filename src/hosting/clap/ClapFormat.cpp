#include "hosting/clap/ClapFormat.h"

#include <clap/clap.h>

#include <cstdint>
#include <cstring>

namespace lattice::hosting
{

namespace
{
// The host a plugin sees while it's described: it asks for nothing and offers nothing.
const void* hostExtension(const clap_host_t*, const char*) { return nullptr; }
void hostRequest(const clap_host_t*) {}

const clap_host_t scanHost{CLAP_VERSION_INIT, nullptr, "Lattice", "Lattice", "", "0.1",
                           hostExtension,     hostRequest, hostRequest, hostRequest};

/** The file the CLAP entry is in: the .clap file itself, or on macOS the bundle's binary. */
juce::File binaryOf(const juce::File& clap)
{
#if JUCE_MAC
    if (clap.isDirectory())
        return clap.getChildFile("Contents/MacOS").getChildFile(clap.getFileNameWithoutExtension());
#endif
    return clap;
}

juce::String categoryOf(const char* const* features, bool& instrument)
{
    instrument = false;
    juce::StringArray main, rest;
    for (auto f = features; f != nullptr && *f != nullptr; ++f)
    {
        const juce::String feature{juce::CharPointer_UTF8(*f)};
        if (feature == CLAP_PLUGIN_FEATURE_INSTRUMENT)
        {
            instrument = true;
            main.add("Instrument");
        }
        else if (feature == CLAP_PLUGIN_FEATURE_AUDIO_EFFECT)
            main.add("Effect");
        else if (feature == CLAP_PLUGIN_FEATURE_NOTE_EFFECT)
            main.add("Note Effect");
        else if (feature == CLAP_PLUGIN_FEATURE_NOTE_DETECTOR)
            main.add("Note Detector");
        else if (feature == CLAP_PLUGIN_FEATURE_ANALYZER)
            main.add("Analyzer");
        else if (feature.isNotEmpty())
        {
            // "phase-vocoder" reads "Phase Vocoder"
            juce::StringArray words;
            for (const auto& w : juce::StringArray::fromTokens(feature, "-_", {}))
                words.add(w.substring(0, 1).toUpperCase() + w.substring(1));
            rest.add(words.joinIntoString(" "));
        }
    }
    main.addArray(rest);
    return main.joinIntoString("|");
}

/** The plugin's ports, from a short-lived instance (CLAP's ports extensions need one). */
void readPorts(const clap_plugin_factory_t& factory, const char* id, scan::ScannedPlugin& out)
{
    const auto* plugin = factory.create_plugin(&factory, &scanHost, id);
    if (plugin == nullptr)
        return;
    if (plugin->init(plugin))
    {
        if (const auto* audio = static_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS)))
        {
            for (const bool input : {true, false})
            {
                int channels = 0;
                const auto n = audio->count(plugin, input);
                for (std::uint32_t i = 0; i < n; ++i)
                {
                    clap_audio_port_info_t info{};
                    if (audio->get(plugin, i, input, &info))
                        channels += static_cast<int>(info.channel_count);
                }
                (input ? out.description.numInputChannels : out.description.numOutputChannels) = channels;
            }
        }
        if (const auto* notes = static_cast<const clap_plugin_note_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_NOTE_PORTS)))
        {
            out.midiInputs = static_cast<int>(notes->count(plugin, true));
            out.midiOutputs = static_cast<int>(notes->count(plugin, false));
            out.mpe = 0;
            for (std::uint32_t i = 0; i < notes->count(plugin, true); ++i)
            {
                clap_note_port_info_t info{};
                if (notes->get(plugin, i, true, &info) && (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI_MPE) != 0)
                    out.mpe = 1;
            }
        }
        else
        {
            out.midiInputs = 0;
            out.midiOutputs = 0;
            out.mpe = 0;
        }
    }
    plugin->destroy(plugin);
}
} // namespace

int ClapPluginFormat::uniqueIdOf(const juce::String& clapId)
{
    // FNV-1a over the UTF-8: the same on every machine and in every JUCE version.
    std::uint32_t h = 2166136261u;
    for (auto p = clapId.toRawUTF8(); *p != 0; ++p)
    {
        h ^= static_cast<unsigned char>(*p);
        h *= 16777619u;
    }
    return static_cast<int>(h);
}

std::vector<scan::ScannedPlugin> ClapPluginFormat::scanFile(const juce::String& path, juce::String& error)
{
    std::vector<scan::ScannedPlugin> result;
    const juce::File file(path);
    juce::DynamicLibrary library;
    if (!library.open(binaryOf(file).getFullPathName()))
    {
        error = "couldn't be loaded";
        return result;
    }
    const auto* entry = static_cast<const clap_plugin_entry_t*>(library.getFunction("clap_entry"));
    if (entry == nullptr)
    {
        error = "not a CLAP plugin (no clap_entry)";
        return result;
    }
    if (!clap_version_is_compatible(entry->clap_version))
    {
        error = "made for CLAP " + juce::String(entry->clap_version.major) + "." + juce::String(entry->clap_version.minor) +
                ", which this version of Lattice can't load";
        return result;
    }
    if (!entry->init(file.getFullPathName().toRawUTF8()))
    {
        error = "failed to start (its entry's init returned false)";
        return result;
    }
    const auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (factory != nullptr)
    {
        const auto count = factory->get_plugin_count(factory);
        for (std::uint32_t i = 0; i < count; ++i)
        {
            const auto* d = factory->get_plugin_descriptor(factory, i);
            if (d == nullptr || d->id == nullptr || d->name == nullptr || !clap_version_is_compatible(d->clap_version))
                continue;
            scan::ScannedPlugin p;
            auto& desc = p.description;
            const auto text = [](const char* s) { return s != nullptr ? juce::String(juce::CharPointer_UTF8(s)) : juce::String(); };
            desc.name = text(d->name);
            desc.descriptiveName = text(d->description).isNotEmpty() ? text(d->description) : desc.name;
            desc.pluginFormatName = formatName;
            desc.manufacturerName = text(d->vendor);
            desc.version = text(d->version);
            desc.fileOrIdentifier = file.getFullPathName();
            desc.lastFileModTime = file.getLastModificationTime();
            desc.lastInfoUpdateTime = juce::Time::getCurrentTime();
            desc.uniqueId = desc.deprecatedUid = uniqueIdOf(text(d->id));
            desc.category = categoryOf(d->features, desc.isInstrument);
            readPorts(*factory, d->id, p);
            result.push_back(std::move(p));
        }
    }
    entry->deinit();
    if (factory == nullptr)
        error = "has no plugin factory";
    return result;
}

void ClapPluginFormat::findAllTypesForFile(juce::OwnedArray<juce::PluginDescription>& results, const juce::String& fileOrIdentifier)
{
    juce::String error;
    for (auto& p : scanFile(fileOrIdentifier, error))
        results.add(new juce::PluginDescription(p.description));
}

bool ClapPluginFormat::fileMightContainThisPluginType(const juce::String& fileOrIdentifier)
{
    const juce::File f(fileOrIdentifier);
#if JUCE_MAC
    return f.hasFileExtension("clap") && (f.existsAsFile() || f.isDirectory());
#else
    return f.hasFileExtension("clap") && f.existsAsFile();
#endif
}

juce::String ClapPluginFormat::getNameOfPluginFromIdentifier(const juce::String& fileOrIdentifier)
{
    return juce::File(fileOrIdentifier).getFileNameWithoutExtension();
}

bool ClapPluginFormat::pluginNeedsRescanning(const juce::PluginDescription& description)
{
    return juce::File(description.fileOrIdentifier).getLastModificationTime() != description.lastFileModTime;
}

bool ClapPluginFormat::doesPluginStillExist(const juce::PluginDescription& description)
{
    return juce::File(description.fileOrIdentifier).exists();
}

juce::StringArray ClapPluginFormat::searchPathsForPlugins(const juce::FileSearchPath& directories, bool recursive, bool)
{
    juce::StringArray found;
    for (int i = 0; i < directories.getNumPaths(); ++i)
    {
        const auto folder = directories.getRawString(i).isNotEmpty() ? juce::File(directories.getRawString(i)) : juce::File();
        if (!folder.isDirectory())
            continue;
        // Not into a .clap bundle (macOS), which is one plugin file.
        std::vector<juce::File> pending{folder};
        while (!pending.empty())
        {
            const auto dir = pending.back();
            pending.pop_back();
            for (const auto& entry : juce::RangedDirectoryIterator(dir, false, "*", juce::File::findFilesAndDirectories))
            {
                const auto& f = entry.getFile();
                if (f.hasFileExtension("clap") && fileMightContainThisPluginType(f.getFullPathName()))
                    found.add(f.getFullPathName());
                else if (recursive && entry.isDirectory() && !entry.isHidden())
                    pending.push_back(f);
            }
        }
    }
    found.sort(true);
    return found;
}

juce::FileSearchPath ClapPluginFormat::getDefaultLocationsToSearch()
{
    // The CLAP spec's folders (clap/entry.h), and CLAP_PATH.
    juce::FileSearchPath paths;
#if JUCE_WINDOWS
    const auto common = juce::SystemStats::getEnvironmentVariable("COMMONPROGRAMFILES", {});
    if (common.isNotEmpty())
        paths.add(juce::File(common).getChildFile("CLAP"));
    const auto local = juce::SystemStats::getEnvironmentVariable("LOCALAPPDATA", {});
    if (local.isNotEmpty())
        paths.add(juce::File(local).getChildFile("Programs").getChildFile("Common").getChildFile("CLAP"));
    constexpr auto separator = ";";
#elif JUCE_MAC
    paths.add(juce::File("/Library/Audio/Plug-Ins/CLAP"));
    paths.add(juce::File("~/Library/Audio/Plug-Ins/CLAP"));
    constexpr auto separator = ":";
#else
    paths.add(juce::File("~/.clap"));
    paths.add(juce::File("/usr/lib/clap"));
    constexpr auto separator = ":";
#endif
    for (const auto& p : juce::StringArray::fromTokens(juce::SystemStats::getEnvironmentVariable("CLAP_PATH", {}), separator, {}))
        if (p.trim().isNotEmpty())
            paths.add(juce::File(p.trim()));
    return paths;
}

void ClapPluginFormat::createPluginInstance(const juce::PluginDescription&, double, int, PluginCreationCallback callback)
{
    callback(nullptr, "CLAP plugins can't be loaded as devices yet (docs/07-m6-plugins-external.md, 6b)");
}

} // namespace lattice::hosting
