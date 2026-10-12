#include "app/MainWindow.h"

namespace lattice
{
namespace
{
constexpr auto deviceStateKey = "audioDeviceState";
constexpr auto toneFrequencyKey = "testToneFrequency";
constexpr auto toneLevelKey = "testToneLevelDb";
constexpr auto multiThreadedKey = "multiThreadedAudio"; // read at startup too (Main.cpp)
constexpr int playPanelHeight = 250;

juce::String percent(double fraction)
{
    return juce::String(fraction * 100.0, 1) + "%";
}
} // namespace

// --- AudioPanel ----------------------------------------------------------------------------------------

AudioPanel::AudioPanel(AudioEngine& e, juce::PropertiesFile& s) : engine(e), settings(s)
{
    // Controls don't take keyboard focus, so letter keys always reach the computer keyboard.
    toneToggle.setWantsKeyboardFocus(false);
    frequencySlider.setWantsKeyboardFocus(false);
    levelSlider.setWantsKeyboardFocus(false);
    resetStatsButton.setWantsKeyboardFocus(false);

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

    // Parallel track processing; off processes everything on the audio thread (for debugging).
    multiThreaded.setWantsKeyboardFocus(false);
    multiThreaded.setToggleState(engine.workerThreads() > 0, juce::dontSendNotification);
    multiThreaded.setTooltip("Process tracks on " + juce::String(WorkerPool::defaultWorkers()) +
                             " worker threads as well as the audio thread");
    multiThreaded.onClick = [this] {
        const bool on = multiThreaded.getToggleState();
        engine.setWorkerThreads(on ? WorkerPool::defaultWorkers() : 0);
        settings.setValue(multiThreadedKey, on);
    };
    addAndMakeVisible(multiThreaded);

    resetStatsButton.onClick = [this] {
        engine.callbackStats().reset();
        engine.inputLatency().reset();
    };
    addAndMakeVisible(resetStatsButton);

    statusLabel.setJustificationType(juce::Justification::topLeft);
    statusLabel.setFont(juce::FontOptions(juce::Font::getDefaultMonospacedFontName(), 14.0f, juce::Font::plain));
    statusLabel.setText("Opening audio device...", juce::dontSendNotification);
    addAndMakeVisible(statusLabel);

    updateToneFromControls();
}

AudioPanel::~AudioPanel()
{
    engine.getDeviceManager().removeChangeListener(this);
    saveDeviceState();
}

void AudioPanel::audioDeviceOpened(const juce::String& error)
{
    deviceError = error;

    // Created only now: building the selector scans the device types, which would otherwise
    // delay the first frame.
    deviceSelector = std::make_unique<juce::AudioDeviceSelectorComponent>(
        engine.getDeviceManager(), 0, 0, 1, AudioEngine::numOutputChannels, true, false, true, false);
    addAndMakeVisible(*deviceSelector);
    engine.getDeviceManager().addChangeListener(this);
    resized();

    timerCallback();
    startTimerHz(4);
}

void AudioPanel::paint(juce::Graphics& g)
{
    g.fillAll(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId));
}

void AudioPanel::resized()
{
    auto area = getLocalBounds().reduced(16);

    auto toneRow = area.removeFromTop(28);
    toneToggle.setBounds(toneRow.removeFromLeft(180));
    resetStatsButton.setBounds(toneRow.removeFromRight(110));
    toneRow.removeFromRight(12);
    multiThreaded.setBounds(toneRow.removeFromRight(200));
    area.removeFromTop(8);

    auto freqRow = area.removeFromTop(28);
    frequencyLabel.setBounds(freqRow.removeFromLeft(90));
    frequencySlider.setBounds(freqRow);
    area.removeFromTop(4);

    auto levelRow = area.removeFromTop(28);
    levelLabel.setBounds(levelRow.removeFromLeft(90));
    levelSlider.setBounds(levelRow);
    area.removeFromTop(12);

    statusLabel.setBounds(area.removeFromTop(170));
    area.removeFromTop(8);

    if (deviceSelector != nullptr)
        deviceSelector->setBounds(area);
}

void AudioPanel::updateToneFromControls()
{
    auto& tone = engine.testTone();
    tone.setFrequency(static_cast<float>(frequencySlider.getValue()));
    tone.setGain(juce::Decibels::decibelsToGain(static_cast<float>(levelSlider.getValue()), -60.0f));
    settings.setValue(toneFrequencyKey, frequencySlider.getValue());
    settings.setValue(toneLevelKey, levelSlider.getValue());
}

void AudioPanel::timerCallback()
{
    auto& dm = engine.getDeviceManager();
    juce::String text;

    if (auto* device = dm.getCurrentAudioDevice())
    {
        const double sr = device->getCurrentSampleRate();
        const int block = device->getCurrentBufferSizeSamples();
        const int outLatency = device->getOutputLatencyInSamples();
        const auto s = engine.callbackStats().summary();
        const auto latency = engine.inputLatency().summary();

        text << "Device:   " << dm.getCurrentAudioDeviceType() << " / " << device->getName() << "\n"
             << "Buffer:   " << block << " samples @ " << juce::String(sr, 0) << " Hz = "
             << juce::String(1000.0 * block / sr, 2) << " ms;  reported output latency "
             << juce::String(1000.0 * outLatency / sr, 2) << " ms\n"
             << "CPU:      " << percent(dm.getCpuUsage()) << " (JUCE average)\n"
             << "Callback: p50 " << percent(s.p50) << "  p99 " << percent(s.p99) << "  p99.9 "
             << percent(s.p999) << "  max " << percent(s.max) << "  of the buffer period  (budget: p99.9 <= 70%)\n"
             << "Overruns: " << juce::String(static_cast<juce::int64>(s.overruns))
             << " callbacks longer than one buffer;  device xruns: " << dm.getXRunCount() << "\n"
             << "Calls:    " << juce::String(static_cast<juce::int64>(s.callbacks)) << ";  worker threads: "
             << engine.workerThreads() << "\n"
             << "Input:    MIDI in -> block handed to the driver: ";
        if (latency.count == 0)
            text << "no notes yet";
        else
            text << "p50 " << juce::String(latency.p50Ms, 1) << " ms  p99 " << juce::String(latency.p99Ms, 1)
                 << " ms  max " << juce::String(latency.maxMs, 2) << " ms  (" << juce::String(static_cast<juce::int64>(latency.count))
                 << " events; Lattice's share, before the driver's output latency)";
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

void AudioPanel::changeListenerCallback(juce::ChangeBroadcaster*)
{
    saveDeviceState();
}

void AudioPanel::saveDeviceState()
{
    if (!persistDeviceState || engine.isOpening() || deviceSelector == nullptr)
        return; // the device manager belongs to the opening thread until audioDeviceOpened()
    if (auto xml = engine.getDeviceManager().createStateXml())
        settings.setValue(deviceStateKey, xml.get());
}

// --- MainComponent -------------------------------------------------------------------------------------

MainComponent::MainComponent(AudioEngine& engine, juce::PropertiesFile& settings, TuningRegistry& tunings,
                             SessionController& session, std::function<void()> firstPaint)
    : onFirstPaint(std::move(firstPaint)), audioPanel(engine, settings), sessionPanel(session, engine, tunings, settings),
      playPanel(session, engine, settings, tunings)
{
    setOpaque(true);
    setWantsKeyboardFocus(true);

    tabs.setWantsKeyboardFocus(false);
    tabs.getTabbedButtonBar().setWantsKeyboardFocus(false);
    const auto background = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    tabs.addTab("Session", background, &sessionPanel, false);
    tabs.addTab("Audio", background, &audioPanel, false);
    tabs.setTabBarDepth(28);
    addAndMakeVisible(tabs);
    addAndMakeVisible(playPanel);

    sessionPanel.onSelectedTrackChanged = [this] { playPanel.setTrack(sessionPanel.selection().track); };
    playPanel.setTrack(sessionPanel.selection().track);

    setSize(1180, 940);
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
    auto area = getLocalBounds().reduced(8);
    playPanel.setBounds(area.removeFromBottom(playPanelHeight));
    area.removeFromBottom(8);
    tabs.setBounds(area);
}

bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    if (dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent()) != nullptr)
        return false; // typing in a text field

    if (sessionPanel.handleKey(key))
        return true;

    const auto mods = key.getModifiers();
    if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown())
        return false;

    auto character = key.getTextCharacter();
    if (character == 0)
        character = static_cast<juce::juce_wchar>(key.getKeyCode());
    return playPanel.computerKeyboard().keyDown(character, key.getKeyCode());
}

bool MainComponent::keyStateChanged(bool)
{
    sessionPanel.handleKeyStateChange();
    auto& keyboard = playPanel.computerKeyboard();
    for (const int code : keyboard.heldKeyCodes())
        if (!juce::KeyPress::isKeyCurrentlyDown(code))
            keyboard.keyUp(code);
    return false;
}

// --- MainWindow ----------------------------------------------------------------------------------------

MainWindow::MainWindow(const juce::String& name, AudioEngine& engine, juce::PropertiesFile& settings,
                       TuningRegistry& tunings, SessionController& session, std::function<void()> onFirstPaint,
                       const std::function<void(const char*)>& markStage)
    : juce::DocumentWindow(name, juce::Colours::black, juce::DocumentWindow::allButtons)
{
    markStage("native window created");
    setUsingNativeTitleBar(true);
    markStage("native title bar");
    component = new MainComponent(engine, settings, tunings, session, std::move(onFirstPaint));
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
    juce::JUCEApplication::getInstance()->systemRequestedQuit(); // asks about unsaved changes
}

void MainWindow::activeWindowStatusChanged()
{
    // Key-up events go to whichever window has focus, so held notes would otherwise hang.
    if (!isActiveWindow() && component != nullptr)
        component->releaseHeldKeys();
}

} // namespace lattice
