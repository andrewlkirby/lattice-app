#pragma once

#include <clap/clap.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>

#include <memory>

namespace lattice::hosting
{

/** Opens a plugin's library with no system dialog: on Windows a file that isn't a library, or one
    whose own libraries are missing, otherwise shows a "Bad Image" box that waits for a click.
    Every format's in-process loading goes through this (docs/07-m6-plugins-external.md, 6a's
    finding). */
bool openLibraryQuietly(juce::DynamicLibrary& library, const juce::File& file);

/** Runs `fn` with Windows' error boxes off for this thread (as openLibraryQuietly does), for
    formats whose libraries JUCE opens itself. Elsewhere it just runs it. */
void withoutErrorBoxes(const std::function<void()>& fn);

/** The file the CLAP entry is in: the .clap file itself, or on macOS the bundle's binary. */
juce::File clapBinaryOf(const juce::File& clap);

/** A .clap file's code, loaded with its entry initialised, for as long as any plugin made from it
    lives: CLAP asks for init() once per load and deinit() before the library is unloaded, so
    instances from one file share one ClapLibrary. Message thread. */
class ClapLibrary
{
public:
    /** The library for a .clap file: the one already loaded, or loaded now. Null with `error` set if
        it can't be. */
    static std::shared_ptr<ClapLibrary> open(const juce::File& file, juce::String& error);
    ~ClapLibrary();

    const clap_plugin_factory_t* factory() const noexcept { return pluginFactory; }
    const juce::File& file() const noexcept { return path; }

private:
    ClapLibrary() = default;

    juce::File path;
    juce::DynamicLibrary library;
    const clap_plugin_entry_t* entry = nullptr;
    const clap_plugin_factory_t* pluginFactory = nullptr;

    JUCE_DECLARE_NON_COPYABLE(ClapLibrary)
};

} // namespace lattice::hosting
