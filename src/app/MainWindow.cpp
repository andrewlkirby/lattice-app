#include "app/MainWindow.h"

namespace lattice
{
namespace
{
constexpr auto deviceStateKey = "audioDeviceState";
constexpr auto toneFrequencyKey = "testToneFrequency";
constexpr auto toneLevelKey = "testToneLevelDb";

juce::String percent(double fraction)
{
    return juce::String(fraction * 100.0, 1) + "%";
}
} // namespace

MainComponent::MainComponent(AudioEngine& e, juce::PropertiesFile& s, std::function<void()> firstPaint)
    : engine(e), settings(s), onFirstPaint(std::move(firstPaint))
{
    setOpaque(true);
    setWantsKeyboardFocus(true);

    toneToggle.onClick = [this] { engine.testTone().setEnabled(toneToggle.getToggleState()); };
    addAndMakeVisible(toneToggle);

    frequencySlider.setRange(20.0, 2000.0, 0.01);
    frequencySlider.setSkewFactorFromMidPoint(200.0);
    frequencySlider.setTextValueSuffix(" Hz");
    frequencySlider.setValue(settings.getDoubleValue(toneFrequencyKey, 440.0), juce::dontSendNotification);
    frequencySlider.onValueChange = [this] { updateToneFromControls(); };
    addAndMakeVisible(frequencySlider);
    addAndMakeVisible(frequencyLabel);

    levelSlider.setRange(-60.0, 0.0, 0.1);
    levelSlider.setTextValueSuffix(" dB");
    levelSlider.setValue(settings.getDoubleValue(toneLevelKey, -12.0), juce::dontSendNotification);
    levelSlider.onValueChange = [this] { updateToneFromControls(); };
    addAndMakeVisible(levelSlider);
    addAndMakeVisible(levelLabel);

    resetStatsButton.onClick = [this] { engine.callbackStats().reset(); };
    addAndMakeVisible(resetStatsButton);

    statusLabel.setJustificationType(juce::Justification::topLeft);
    statusLabel.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 14.0f, juce::Font::plain));
    statusLabel.setText("Opening audio device...", juce::dontSendNotification);
    addAndMakeVisible(statusLabel);

    updateToneFromControls();
    setSize(900, 680);
}

MainComponent::~MainComponent()
{
    engine.getDeviceManager().removeChangeListener(this);
    saveDeviceState();
}

void MainComponent::audioDeviceOpened(const juce::String& error)
{
    deviceError = error;

    // Created only now: building the selector scans the device types, which would otherwise
    // delay the first frame.
    deviceSelector = std::make_unique<juce::AudioDeviceSelectorComponent>(
        engine.getDeviceManager(), 0, 0, 1, AudioEngine::numOutputChannels, false, false, true, false);
    addAndMakeVisible(*deviceSelector);
    engine.getDeviceManager().addChangeListener(this);
    resized();

    timerCallback();
    startTimerHz(4);
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));

    if (!firstPaintDone)
    {
        firstPaintDone = true;
        if (onFirstPaint)
            juce::MessageManager::callAsync(onFirstPaint);
    }
}

void MainComponent::resized()
{
    auto area = getLocalBounds().reduced(16);

    auto toneRow = area.removeFromTop(28);
    toneToggle.setBounds(toneRow.removeFromLeft(180));
    resetStatsButton.setBounds(toneRow.removeFromRight(110));
    area.removeFromTop(8);

    auto freqRow = area.removeFromTop(28);
    frequencyLabel.setBounds(freqRow.removeFromLeft(90));
    frequencySlider.setBounds(freqRow);
    area.removeFromTop(4);

    auto levelRow = area.removeFromTop(28);
    levelLabel.setBounds(levelRow.removeFromLeft(90));
    levelSlider.setBounds(levelRow);
    area.removeFromTop(12);

    statusLabel.setBounds(area.removeFromTop(150));
    area.removeFromTop(8);

    if (deviceSelector != nullptr)
        deviceSelector->setBounds(area);
}

bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::spaceKey)
    {
        toneToggle.setToggleState(!toneToggle.getToggleState(), juce::sendNotification);
        return true;
    }
    return false;
}

void MainComponent::updateToneFromControls()
{
    auto& tone = engine.testTone();
    tone.setFrequency(static_cast<float>(frequencySlider.getValue()));
    tone.setGain(juce::Decibels::decibelsToGain(static_cast<float>(levelSlider.getValue()), -60.0f));
    settings.setValue(toneFrequencyKey, frequencySlider.getValue());
    settings.setValue(toneLevelKey, levelSlider.getValue());
}

void MainComponent::timerCallback()
{
    auto& dm = engine.getDeviceManager();
    juce::String text;

    if (auto* device = dm.getCurrentAudioDevice())
    {
        const double sr = device->getCurrentSampleRate();
        const int block = device->getCurrentBufferSizeSamples();
        const int outLatency = device->getOutputLatencyInSamples();
        const auto s = engine.callbackStats().summary();

        text << "Device:   " << dm.getCurrentAudioDeviceType() << " / " << device->getName() << "\n"
             << "Buffer:   " << block << " samples @ " << juce::String(sr, 0) << " Hz = "
             << juce::String(1000.0 * block / sr, 2) << " ms;  reported output latency "
             << juce::String(1000.0 * outLatency / sr, 2) << " ms\n"
             << "CPU:      " << percent(dm.getCpuUsage()) << " (JUCE average)\n"
             << "Callback: p50 " << percent(s.p50) << "  p99 " << percent(s.p99) << "  p99.9 "
             << percent(s.p999) << "  max " << percent(s.max) << "  of the buffer period  (budget: p99.9 <= 70%)\n"
             << "Overruns: " << juce::String(static_cast<juce::int64>(s.overruns))
             << " callbacks longer than one buffer;  device xruns: " << dm.getXRunCount() << "\n"
             << "Calls:    " << juce::String(static_cast<juce::int64>(s.callbacks));
#if LATTICE_RT_CHECKS_ENABLED
        text << "\nRT checks: on, violations so far: "
             << juce::String(static_cast<juce::int64>(rt::violationCount()));
#endif
    }
    else
    {
        text << "No audio device open." << (deviceError.isNotEmpty() ? "\n" + deviceError : juce::String());
    }

    statusLabel.setText(text, juce::dontSendNotification);
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster*)
{
    saveDeviceState();
}

void MainComponent::saveDeviceState()
{
    if (!persistDeviceState || engine.isOpening() || deviceSelector == nullptr)
        return; // the device manager belongs to the opening thread until audioDeviceOpened()
    if (auto xml = engine.getDeviceManager().createStateXml())
        settings.setValue(deviceStateKey, xml.get());
}

MainWindow::MainWindow(const juce::String& name, AudioEngine& engine, juce::PropertiesFile& settings,
                       std::function<void()> onFirstPaint, const std::function<void(const char*)>& markStage)
    : juce::DocumentWindow(name, juce::Colours::black, juce::DocumentWindow::allButtons)
{
    markStage("native window created");
    setUsingNativeTitleBar(true);
    markStage("native title bar");
    component = new MainComponent(engine, settings, std::move(onFirstPaint));
    markStage("content created");
    setContentOwned(component, true);
    setResizable(true, true);
    centreWithSize(getWidth(), getHeight());
    markStage("window laid out");
    setVisible(true);
    markStage("window visible");
    component->grabKeyboardFocus();
}

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}

} // namespace lattice
