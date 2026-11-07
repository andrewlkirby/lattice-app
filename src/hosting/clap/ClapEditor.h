#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <memory>

namespace lattice::hosting
{

class ClapPluginInstance;

/** A CLAP plugin's own window, embedded (docs/07-m6-plugins-external.md, 6b, "Plugin windows"):
    the plugin draws into a native child window that follows this component (an HWND on Windows,
    an XEmbed window on Linux).

    - Its size is the plugin's, in logical pixels; on Windows the plugin works in physical pixels,
      so it's told the window's scale (the monitor's, times the app's) and told again when the
      window moves to a monitor with another scale.
    - A plugin that can be resized follows the window, through adjust_size(); one that asks for a
      new size gets it.
    - The plugin's window is destroyed with this one, which frees what it holds.

    Message thread. */
class ClapEditor final : public juce::AudioProcessorEditor, private juce::ComponentPeer::ScaleFactorListener
{
public:
    ClapEditor(ClapPluginInstance& owner, const char* api);
    ~ClapEditor() override;

    /** The plugin asked for this size (physical pixels on Windows). */
    void pluginAskedForSize(std::uint32_t width, std::uint32_t height);

    void paint(juce::Graphics& g) override;
    void resized() override;
    void parentHierarchyChanged() override;
    void visibilityChanged() override;

private:
    void attachIfReady();
    double scale() const;
    void setSizeFromPlugin(std::uint32_t width, std::uint32_t height);
    void nativeScaleFactorChanged(double newScaleFactor) override;
    void watchPeer(juce::ComponentPeer* peer);

    ClapPluginInstance& owner;
    const char* api;
    bool created = false, attached = false, resizable = false, applyingPluginSize = false;
    double currentScale = 1.0;
    juce::ComponentPeer* watchedPeer = nullptr;
    std::unique_ptr<juce::Component> host; // the native window the plugin draws in
    void* hostWindow = nullptr;            // its handle

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ClapEditor)
};

/** The native child window a plugin's GUI is placed in, as a component, and its handle (null if
    this OS has none). */
std::unique_ptr<juce::Component> makeNativeHost(void*& handle);

} // namespace lattice::hosting
