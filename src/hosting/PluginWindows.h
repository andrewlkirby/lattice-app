#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_data_structures/juce_data_structures.h>

#include <functional>
#include <map>
#include <memory>

namespace lattice::hosting
{

class HostedPluginDevice;

/** Plugins' own windows (docs/07-m6-plugins-external.md, 6b, "Plugin windows"): floating, one per
    device, where it was last (the device's pluginWindow, kept without an undo step), optionally
    always on top. A closed window is deleted, and its editor with it, which frees what the plugin's
    GUI holds; a window goes before its plugin does. On Windows each window follows its monitor's
    scale (JUCE tells a VST3 editor; a CLAP editor follows on its own). Message thread. */
class PluginWindows
{
public:
    PluginWindows();
    ~PluginWindows();

    /** Opens the device's window, or brings it to the front. False if the plugin has none. */
    bool open(HostedPluginDevice& device, juce::ValueTree deviceNode);
    void close(std::int64_t deviceId);
    void closeAll();
    bool isOpen(std::int64_t deviceId) const { return windows.contains(deviceId); }
    /** The window of a device, if open (tests, screenshots). */
    juce::DocumentWindow* windowFor(std::int64_t deviceId) const;

    void setAlwaysOnTop(bool on);
    bool alwaysOnTop() const noexcept { return onTop; }

    /** Called when always-on-top changes (the app keeps it). */
    std::function<void(bool)> onAlwaysOnTopChanged;
    /** Called when a window opens or closes. */
    std::function<void()> onChange;

private:
    class Window;
    std::map<std::int64_t, std::unique_ptr<Window>> windows;
    bool onTop = false;
};

} // namespace lattice::hosting
