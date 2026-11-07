// A CLAP test plugin file (docs/07-m6-plugins-external.md, "Tests" 1): two plugins in one file, as
// CLAP allows. A synth (stereo out, a note input taking MPE) and a note effect (no audio, notes in
// and out), so a scan has descriptors, features and ports to read. The synth has the name and vendor
// of the JUCE-built "Lattice Test Plugin", so the two are one plugin in two formats. They make no
// sound: hosting them comes in 6b.

#include <clap/clap.h>

#include <cstring>

#if defined(_WIN32)
#define LATTICE_EXPORT extern "C" __declspec(dllexport)
#else
#define LATTICE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace
{
const char* const synthFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO,
                                     nullptr};
const char* const arpFeatures[] = {CLAP_PLUGIN_FEATURE_NOTE_EFFECT, "arpeggiator", nullptr};

const clap_plugin_descriptor_t synthDescriptor{CLAP_VERSION_INIT, "org.lattice.tests.clap-synth", "Lattice Test Plugin",
                                               "Lattice Tests", "", "", "", "1.2.3", "A CLAP synth that makes no sound",
                                               synthFeatures};
const clap_plugin_descriptor_t arpDescriptor{CLAP_VERSION_INIT, "org.lattice.tests.clap-arp", "Lattice Test Clap Arp",
                                             "Lattice Tests", "", "", "", "0.9", "", arpFeatures};

bool isSynth(const clap_plugin_t* plugin) { return plugin->desc == &synthDescriptor; }

// --- Ports ---------------------------------------------------------------------------------------
std::uint32_t audioCount(const clap_plugin_t* plugin, bool isInput) { return isSynth(plugin) && !isInput ? 1u : 0u; }
bool audioGet(const clap_plugin_t* plugin, std::uint32_t index, bool isInput, clap_audio_port_info_t* info)
{
    if (!isSynth(plugin) || isInput || index != 0)
        return false;
    info->id = 0;
    std::strcpy(info->name, "Out");
    info->flags = CLAP_AUDIO_PORT_IS_MAIN;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t audioPorts{audioCount, audioGet};

std::uint32_t noteCount(const clap_plugin_t* plugin, bool isInput) { return isInput || !isSynth(plugin) ? 1u : 0u; }
bool noteGet(const clap_plugin_t* plugin, std::uint32_t index, bool isInput, clap_note_port_info_t* info)
{
    if (index != 0 || (!isInput && isSynth(plugin)))
        return false;
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_MIDI | (isSynth(plugin) && isInput ? CLAP_NOTE_DIALECT_MIDI_MPE : 0u);
    info->preferred_dialect = CLAP_NOTE_DIALECT_MIDI;
    std::strcpy(info->name, isInput ? "Notes In" : "Notes Out");
    return true;
}
const clap_plugin_note_ports_t notePorts{noteCount, noteGet};

// --- The plugin ----------------------------------------------------------------------------------
bool pluginInit(const clap_plugin_t*) { return true; }
void pluginDestroy(const clap_plugin_t* plugin) { delete plugin; }
bool pluginActivate(const clap_plugin_t*, double, std::uint32_t, std::uint32_t) { return true; }
void pluginDeactivate(const clap_plugin_t*) {}
bool pluginStartProcessing(const clap_plugin_t*) { return true; }
void pluginStopProcessing(const clap_plugin_t*) {}
void pluginReset(const clap_plugin_t*) {}
clap_process_status pluginProcess(const clap_plugin_t*, const clap_process_t*) { return CLAP_PROCESS_SLEEP; }
const void* pluginExtension(const clap_plugin_t*, const char* id)
{
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
        return &audioPorts;
    if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0)
        return &notePorts;
    return nullptr;
}
void pluginOnMainThread(const clap_plugin_t*) {}

// --- The factory and the entry -------------------------------------------------------------------
std::uint32_t factoryCount(const clap_plugin_factory_t*) { return 2; }
const clap_plugin_descriptor_t* factoryDescriptor(const clap_plugin_factory_t*, std::uint32_t index)
{
    return index == 0 ? &synthDescriptor : index == 1 ? &arpDescriptor : nullptr;
}
const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t*, const char* id)
{
    const clap_plugin_descriptor_t* d = std::strcmp(id, synthDescriptor.id) == 0 ? &synthDescriptor
                                        : std::strcmp(id, arpDescriptor.id) == 0 ? &arpDescriptor
                                                                                 : nullptr;
    if (d == nullptr)
        return nullptr;
    return new clap_plugin_t{d,
                             nullptr,
                             pluginInit,
                             pluginDestroy,
                             pluginActivate,
                             pluginDeactivate,
                             pluginStartProcessing,
                             pluginStopProcessing,
                             pluginReset,
                             pluginProcess,
                             pluginExtension,
                             pluginOnMainThread};
}
const clap_plugin_factory_t factory{factoryCount, factoryDescriptor, factoryCreate};

bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* entryFactory(const char* id) { return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr; }
} // namespace

LATTICE_EXPORT const clap_plugin_entry_t clap_entry{CLAP_VERSION_INIT, entryInit, entryDeinit, entryFactory};
