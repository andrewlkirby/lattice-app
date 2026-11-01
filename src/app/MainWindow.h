#pragma once

#include "control/ComputerKeyboardInput.h"
#include "engine/AudioEngine.h"
#include "engine/SessionController.h"
#include "tuning/TuningRegistry.h"
#include "ui/session/KeyboardFollow.h"
#include "ui/shell/MainShell.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>

namespace lattice
{

/** The window's content: the shell (ui::MainShell). Keys go to the shell's commands first; what no
    command takes plays the computer MIDI keyboard, which this owns: it plays into the engine's live
    MIDI input, set up for the selected track's tuning (and a Drum Rack's pads, KeyboardFollow). It
    also remembers the audio device setup whenever it changes. */
class MainComponent final : public juce::Component,
                            public juce::DragAndDropContainer,
                            private ui::SessionSelectionModel::Listener,
                            private juce::ValueTree::Listener,
                            private juce::ChangeListener,
                            private TuningRegistry::Listener
{
public:
    MainComponent(AudioEngine& engine, juce::PropertiesFile& settings, TuningRegistry& tunings,
                  SessionController& session, std::function<void()> onFirstPaint);
    ~MainComponent() override;

    /** Called once the device has been opened (successfully or not). */
    void audioDeviceOpened(const juce::String& error);

    /** When false, device changes are not written to the settings (used by test runs such as
        --soak, which change the buffer size temporarily). */
    void setPersistDeviceState(bool shouldPersist) noexcept { persistDeviceState = shouldPersist; }

    ui::MainShell& shell() noexcept { return *mainShell; }
    ComputerKeyboardInput& computerKeyboard() noexcept { return keyboard; }

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;
    bool keyStateChanged(bool isKeyDown) override;

    /** Releases notes held by the computer keyboard (e.g. when the window loses focus). */
    void releaseHeldKeys() { keyboard.releaseAll(); }

private:
    void selectionChanged(const ui::SessionSelection& now) override;
    /** A Drum Rack's page scrolled in the Device view, or the selected track's tuning changed. */
    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override;
    void changeListenerCallback(juce::ChangeBroadcaster*) override; // the audio device changed
    void tuningsChanged() override;
    /** The keyboard set up for the selected track: its tuning's range (again if `retune`), then a
        Drum Rack's pads. */
    void followSelectedTrack(bool retune);

    std::function<void()> onFirstPaint;
    bool firstPaintDone = false;
    bool persistDeviceState = true;
    bool deviceOpen = false;
    AudioEngine& engine;
    juce::PropertiesFile& settings;
    TuningRegistry& tunings;
    SessionController& sessionController;
    ComputerKeyboardInput keyboard;
    TuningPtr keyboardTuning; // what the keyboard is set up for
    ui::KeyboardFollow keyboardFollow;

    std::unique_ptr<ui::MainShell> mainShell;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

class MainWindow final : public juce::DocumentWindow
{
public:
    /** markStage records startup stages (see StartupTimeline). */
    MainWindow(const juce::String& name, AudioEngine& engine, juce::PropertiesFile& settings,
               TuningRegistry& tunings, SessionController& session, std::function<void()> onFirstPaint,
               const std::function<void(const char*)>& markStage);

    ~MainWindow() override;

    MainComponent& content() noexcept { return *component; }
    void closeButtonPressed() override;
    void activeWindowStatusChanged() override;

private:
    MainComponent* component = nullptr; // owned by the window

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};

} // namespace lattice
