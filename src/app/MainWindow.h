#pragma once

#include "app/PlayPanel.h"
#include "app/debug/SessionView.h"
#include "engine/AudioEngine.h"
#include "engine/SessionController.h"
#include "tuning/TuningRegistry.h"
#include "ui/shell/MainShell.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>

namespace lattice
{

/** The "Audio" tab: test tone, callback load and input latency readouts, and the audio/MIDI device
    setup (the M0 main view). */
class AudioPanel final : public juce::Component, private juce::Timer, private juce::ChangeListener
{
public:
    AudioPanel(AudioEngine& engine, juce::PropertiesFile& settings);
    ~AudioPanel() override;

    void audioDeviceOpened(const juce::String& error);
    void setPersistDeviceState(bool shouldPersist) noexcept { persistDeviceState = shouldPersist; }
    void setToneToggle(bool on) { toneToggle.setToggleState(on, juce::dontSendNotification); }

    void paint(juce::Graphics& g) override;
    void resized() override;

private:
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void saveDeviceState();
    void updateToneFromControls();

    AudioEngine& engine;
    juce::PropertiesFile& settings;
    bool persistDeviceState = true;

    juce::ToggleButton toneToggle{"Test tone"};
    juce::ToggleButton multiThreaded{"Multi-threaded audio"};
    juce::Slider frequencySlider;
    juce::Slider levelSlider;
    juce::Label frequencyLabel{{}, "Frequency"};
    juce::Label levelLabel{{}, "Level"};
    juce::TextButton resetStatsButton{"Reset stats"};
    juce::Label statusLabel;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> deviceSelector;
    juce::String deviceError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioPanel)
};

/** The M2/M3 debug UI, as one panel for the shell's "Debug UI" tab until the M4 views replace it
    (docs/05-m4-ui.md, "Order of work"): the debug session view and the audio tab, with the play
    panel (tuning, computer keyboard, on-screen keyboard) underneath. */
class DebugUiPanel final : public juce::Component
{
public:
    DebugUiPanel(AudioEngine& engine, juce::PropertiesFile& settings, TuningRegistry& tunings, SessionController& session);

    AudioPanel& audioPanel() noexcept { return audio; }
    debugui::SessionView& sessionView() noexcept { return sessionPanel; }
    PlayPanel& playPanel() noexcept { return play; }

    void resized() override;

private:
    AudioPanel audio;
    debugui::SessionView sessionPanel;
    juce::TabbedComponent tabs{juce::TabbedButtonBar::TabsAtTop};
    PlayPanel play;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DebugUiPanel)
};

/** The window's content: the M4 shell (ui::MainShell), with the debug UI in its Debug UI tab.
    Keys go to the shell's commands first; what no command takes plays the computer MIDI keyboard. */
class MainComponent final : public juce::Component, public juce::DragAndDropContainer
{
public:
    MainComponent(AudioEngine& engine, juce::PropertiesFile& settings, TuningRegistry& tunings,
                  SessionController& session, std::function<void()> onFirstPaint);
    ~MainComponent() override;

    /** Called once the device has been opened (successfully or not). */
    void audioDeviceOpened(const juce::String& error) { debug.audioPanel().audioDeviceOpened(error); }

    /** When false, device changes are not written to the settings (used by test runs such as
        --soak, which change the buffer size temporarily). */
    void setPersistDeviceState(bool shouldPersist) noexcept { debug.audioPanel().setPersistDeviceState(shouldPersist); }

    /** Reflects a tone state that was changed from outside the UI. */
    void setToneToggle(bool on) { debug.audioPanel().setToneToggle(on); }

    debugui::SessionView& sessionView() noexcept { return debug.sessionView(); }
    ui::MainShell& shell() noexcept { return *mainShell; }

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;
    bool keyStateChanged(bool isKeyDown) override;

    /** Releases notes held by the computer keyboard (e.g. when the window loses focus). */
    void releaseHeldKeys() { debug.playPanel().computerKeyboard().releaseAll(); }

private:
    std::function<void()> onFirstPaint;
    bool firstPaintDone = false;

    DebugUiPanel debug;
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
