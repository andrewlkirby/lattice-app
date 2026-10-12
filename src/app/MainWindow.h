#pragma once

#include "engine/AudioEngine.h"

#include <juce_audio_utils/juce_audio_utils.h>
#include <juce_gui_extra/juce_gui_extra.h>

#include <functional>
#include <memory>

namespace lattice
{

/** M0 main view: audio device setup, test-tone controls, and the callback-load readout. */
class MainComponent final : public juce::Component, private juce::Timer, private juce::ChangeListener
{
public:
    MainComponent(AudioEngine& engine, juce::PropertiesFile& settings, std::function<void()> onFirstPaint);
    ~MainComponent() override;

    /** Called once the device has been opened (successfully or not). */
    void audioDeviceOpened(const juce::String& error);

    /** When false, device changes are not written to the settings (used by test runs such as
        --soak, which change the buffer size temporarily). */
    void setPersistDeviceState(bool shouldPersist) noexcept { persistDeviceState = shouldPersist; }

    /** Reflects a tone state that was changed from outside the UI. */
    void setToneToggle(bool on) { toneToggle.setToggleState(on, juce::dontSendNotification); }

    void paint(juce::Graphics& g) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress& key) override;

private:
    void timerCallback() override;
    void changeListenerCallback(juce::ChangeBroadcaster* source) override;
    void saveDeviceState();
    void updateToneFromControls();

    AudioEngine& engine;
    juce::PropertiesFile& settings;
    std::function<void()> onFirstPaint;
    bool firstPaintDone = false;
    bool persistDeviceState = true;

    juce::ToggleButton toneToggle{"Test tone (space)"};
    juce::Slider frequencySlider;
    juce::Slider levelSlider;
    juce::Label frequencyLabel{{}, "Frequency"};
    juce::Label levelLabel{{}, "Level"};
    juce::TextButton resetStatsButton{"Reset stats"};
    juce::Label statusLabel;
    std::unique_ptr<juce::AudioDeviceSelectorComponent> deviceSelector;
    juce::String deviceError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainComponent)
};

class MainWindow final : public juce::DocumentWindow
{
public:
    /** markStage records startup stages (see StartupTimeline). */
    MainWindow(const juce::String& name, AudioEngine& engine, juce::PropertiesFile& settings,
               std::function<void()> onFirstPaint, const std::function<void(const char*)>& markStage);

    MainComponent& content() noexcept { return *component; }
    void closeButtonPressed() override;

private:
    MainComponent* component = nullptr; // owned by the window

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(MainWindow)
};

} // namespace lattice
