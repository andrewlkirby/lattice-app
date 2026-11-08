#pragma once

#include "control/ComputerKeyboardInput.h"
#include "engine/SessionController.h"
#include "library/Library.h"
#include "ui/commands/CommandRegistry.h"
#include "ui/theme/Theme.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace lattice::ui
{
class SurfaceManager;
}
namespace lattice::hosting
{
class PluginLibrary;
}
namespace lattice::mts
{
class Master;
}

namespace lattice::ui
{

/** The Preferences window's content (docs/05-m4-ui.md, 4g): a list of tabs on the left, the tab on
    the right. Every change applies at once and is saved in the settings file.

    - Audio: the device, sample rate and buffer size (JUCE's selector), the latencies (the
      driver's and the measured MIDI-to-sound), the audio load, and a test tone.
    - MIDI: each input's Track switch (its notes reach armed tracks) and Remote switch (MIDI Map
      listens to it); clock sync comes in M6.
    - Controllers: Virtual Push's grid size and pad velocity (docs/06-m5-controllers.md, 5a), and
      the control surfaces on MIDI ports, with the Surface Monitor (5e).
    - Look & Feel: the theme (built in, or a JSON file in the Themes folder) and the zoom.
    - Library: the browser's places, adding, removing and rescanning them.
    - Plugins: the folders the plugin scan searches, scanning, the plugins blocked or failed (with
      why, and a retry), the preferred format and how the browser groups them
      (docs/07-m6-plugins-external.md, 6a).
    - CPU: multi-threaded audio and the number of worker threads.
    - Tuning: new tracks' tuning and high-note policy.
    - Computer Keyboard: the 17 note keys (another layout's row) and each tuning's range step.
    - Shortcuts: every command's keys (JUCE's editor: it asks before taking a key another
      command has), import and export. */
class PreferencesView final : public juce::Component
{
public:
    struct Context
    {
        SessionController& session;
        AudioEngine& engine;
        juce::PropertiesFile& settings;
        CommandRegistry& commands;
        TuningRegistry& tunings;
        library::Library& library;
        ComputerKeyboardInput* keyboard = nullptr;
        std::function<void(const Theme&)> setTheme = {};
        std::function<const Theme&()> theme = {};
        std::function<void()> keyboardChanged = {}; // the note keys or a range step changed
        std::function<void()> midiChanged = {};     // an input's Track or Remote switch changed
        std::function<void()> controllersChanged = {}; // Virtual Push's settings changed
        std::function<void(std::function<void(const juce::File&)>)> chooseFolder = {};
        /** A file to import (save false) or export to. */
        std::function<void(const juce::String& title, const juce::String& patterns, bool save, std::function<void(const juce::File&)>)> chooseFile = {};
        juce::File themesFolder = {};
        SurfaceManager* surfaces = nullptr; // the control surfaces (none in some tests)
        hosting::PluginLibrary* plugins = nullptr; // the plugin scan (none in some tests)
        mts::Master* tuningMaster = nullptr;       // MTS-ESP (none in some tests)
        juce::String audioError = {}; // why the audio device didn't open, if it didn't
    };

    enum class Page
    {
        audio,
        midi,
        controllers,
        look,
        library,
        plugins,
        cpu,
        tuning,
        keyboard,
        shortcuts
    };
    static constexpr int numPages = 10;
    static juce::String pageName(Page p);

    explicit PreferencesView(Context context);
    ~PreferencesView() override;

    void show(Page p);
    Page page() const noexcept { return shown; }
    /** The tab showing (tests reach its controls through it). */
    juce::Component* pageComponent() noexcept { return current.get(); }

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;

    static constexpr auto lastPageKey = "prefs.page";

private:
    Context context;
    Page shown = Page::audio;
    std::unique_ptr<juce::Component> current;
    juce::Rectangle<int> tabs;
};

} // namespace lattice::ui
