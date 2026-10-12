#pragma once

// Platform interfaces. Everything OS-specific lives behind these declarations, with one
// implementation per OS in src/platform/{windows,linux,macos}/ (docs/00-preliminary-plan.md,
// "Platforms"). M0 covers folders, process start time, and audio device-type preferences.

#include <juce_core/juce_core.h>

#include <functional>
#include <optional>
#include <string>

namespace lattice::platform
{

struct Paths
{
    juce::File userData; // <userdata>: %APPDATA%/Lattice, $XDG_DATA_HOME/Lattice, ~/Library/Application Support/Lattice
    juce::File settings; // %APPDATA%/Lattice, $XDG_CONFIG_HOME/Lattice, ~/Library/Application Support/Lattice

    juce::File logs() const { return userData.getChildFile("logs"); }
    juce::File settingsFile() const { return settings.getChildFile("Lattice.settings"); }
};

/** The folders for the current user on this OS. Folders are not created here. */
Paths defaultPaths();

/** Seconds since the OS created this process (not since main()), so startup measurements include
    loader and static-initialisation time. */
double secondsSinceProcessStart();

/** Audio device types in order of preference, by JUCE type name. The first one that exists and
    opens is used when no device setup has been saved (docs/perf/reference-machine.md). */
juce::StringArray preferredAudioDeviceTypes();

/** Prepares the calling (non-message) thread for the OS audio device APIs for its lifetime.
    On Windows this joins the COM multithreaded apartment that WASAPI needs; elsewhere it does
    nothing. */
class ScopedAudioDeviceThread
{
public:
    ScopedAudioDeviceThread();
    ~ScopedAudioDeviceThread(); // NOLINT(performance-trivially-destructible): releases COM on Windows
    ScopedAudioDeviceThread(const ScopedAudioDeviceThread&) = delete;
    ScopedAudioDeviceThread& operator=(const ScopedAudioDeviceThread&) = delete;

private:
    [[maybe_unused]] bool needsRelease = false; // used on Windows only
};

/** True if this device type must be opened on the message thread (e.g. ASIO drivers, which are
    single-threaded COM objects bound to the thread that creates them). */
bool audioDeviceTypeNeedsMessageThread(const juce::String& typeName);

namespace detail
{
using EnvLookup = std::function<std::optional<std::string>(const char* name)>;

struct XdgDirs
{
    std::string userData; // POSIX paths
    std::string settings;
};

/** The XDG Base Directory rules used on Linux. Works on plain strings so it can be tested on
    every OS. */
XdgDirs resolveXdgDirs(const EnvLookup& env, const std::string& home);
} // namespace detail

} // namespace lattice::platform
