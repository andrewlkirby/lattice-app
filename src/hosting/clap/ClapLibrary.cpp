#include "hosting/clap/ClapLibrary.h"

#include <map>

#if JUCE_WINDOWS
// Rather than all of windows.h: the one call used.
extern "C" __declspec(dllimport) int __stdcall SetThreadErrorMode(unsigned long newMode, unsigned long* oldMode);
#endif

namespace lattice::hosting
{

void withoutErrorBoxes(const std::function<void()>& fn)
{
#if JUCE_WINDOWS
    constexpr unsigned long failCriticalErrors = 0x0001, noOpenFileErrorBox = 0x8000;
    unsigned long old = 0;
    const bool set = SetThreadErrorMode(failCriticalErrors | noOpenFileErrorBox, &old) != 0;
    fn();
    if (set)
        SetThreadErrorMode(old, nullptr);
#else
    fn();
#endif
}

bool openLibraryQuietly(juce::DynamicLibrary& library, const juce::File& file)
{
    bool opened = false;
    withoutErrorBoxes([&] { opened = library.open(file.getFullPathName()); });
    return opened;
}

juce::File clapBinaryOf(const juce::File& clap)
{
#if JUCE_MAC
    if (clap.isDirectory())
        return clap.getChildFile("Contents/MacOS").getChildFile(clap.getFileNameWithoutExtension());
#endif
    return clap;
}

std::shared_ptr<ClapLibrary> ClapLibrary::open(const juce::File& file, juce::String& error)
{
    JUCE_ASSERT_MESSAGE_THREAD
    static std::map<juce::String, std::weak_ptr<ClapLibrary>> loaded;
    const auto key = file.getFullPathName();
    if (const auto it = loaded.find(key); it != loaded.end())
        if (auto existing = it->second.lock())
            return existing;

    std::shared_ptr<ClapLibrary> lib(new ClapLibrary());
    lib->path = file;
    if (!openLibraryQuietly(lib->library, clapBinaryOf(file)))
    {
        error = "couldn't be loaded";
        return nullptr;
    }
    const auto* entry = static_cast<const clap_plugin_entry_t*>(lib->library.getFunction("clap_entry"));
    if (entry == nullptr)
    {
        error = "not a CLAP plugin (no clap_entry)";
        return nullptr;
    }
    if (!clap_version_is_compatible(entry->clap_version))
    {
        error = "made for a CLAP version this version of Lattice can't load";
        return nullptr;
    }
    bool started = false;
    withoutErrorBoxes([&] { started = entry->init(file.getFullPathName().toRawUTF8()); });
    if (!started)
    {
        error = "failed to start (its entry's init returned false)";
        return nullptr;
    }
    lib->entry = entry;
    lib->pluginFactory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (lib->pluginFactory == nullptr)
    {
        error = "has no plugin factory";
        return nullptr; // deinit() in the destructor
    }
    loaded[key] = lib;
    return lib;
}

ClapLibrary::~ClapLibrary()
{
    if (entry != nullptr)
        entry->deinit();
    library.close();
}

} // namespace lattice::hosting
