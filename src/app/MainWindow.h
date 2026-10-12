#pragma once

#include "app/PlayPanel.h"
#include "app/debug/SessionView.h"
#include "engine/AudioEngine.h"
#include "engine/SessionController.h"
#include "tuning/TuningRegistry.h"

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

/** Main view until M4: the debug session view and the audio tab, with the play panel (tuning,
    computer keyboard, on-screen keyboard) underneath. Routes key presses: session shortcuts first,
    then the computer MIDI keyboard. */
class MainComponent final : public juce::Component
{
public:
    MainComponent(AudioEngine& engine, juce::PropertiesFile& settings, TuningRegistry& tunings,
                  SessionController& session, std::function<void()> onFirstPaint);

    /** Called once the device has been opened (successfully or not). */
    void audioDeviceOpened(const juce::String& error) { audioPanel.audioDeviceOpened(error); }

    /** When false, device changes are not written to the settings (used by test runs such as
        --soak, which change the buffer size temporarily). */
    void setPersistDeviceState(bool shouldPersist) noexcept { audioPanel.setPersistDeviceState(shouldPersist); }

    /** Reflects a tone state that was changed from outside the UI. */
    void setToneToggle(bool on) { audioPanel.setToneToggle(on); }

    debugui::SessionView& sessionView() noexcept { return sessionPanel; }

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;
    bool keyStateChanged(bool isKeyDown) override;

    /** Releases notes held by the computer keyboard (e.g. when the window loses focus). */
    void releaseHeldKeys() { playPanel.computerKeyboard().releaseAll(); }

private:
    std::function<void()> onFirstPaint;
    bool firstPaintDone = false;

    AudioPanel audioPanel;
    debugui::SessionView sessionPanel;
    juce::TabbedComponent tabs{juce::TabbedButtonBar::TabsAtTop};
    PlayPanel playPanel;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

class MainWindow final : public juce::DocumentWindow
{
public:
    /** markStage records startup stages (see StartupTimeline). */
    MainWindow(const juce::String& name, AudioEngine& engine, juce::PropertiesFile& settings,
               TuningRegistry& tunings, SessionController& session, std::function<void()> onFirstPaint,
               const std::function<void(const char*)>& markStage);

    MainComponent& content() noexcept { return *component; }
    void closeButtonPressed() override;
    void activeWindowStatusChanged() override;

private:
    MainComponent* component = nullptr; // owned by the window

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};

} // namespace lattice
