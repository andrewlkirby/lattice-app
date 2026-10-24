#include "app/MainWindow.h"

#include "devices/dsp/Simd.h"

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
             << engine.workerThreads() << ";  SIMD: " << dsp::simdLevelName(dsp::simdLevel()) << "\n"
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

// --- DebugUiPanel --------------------------------------------------------------------------------------

DebugUiPanel::DebugUiPanel(AudioEngine& engine, juce::PropertiesFile& settings, TuningRegistry& tunings,
                           SessionController& session)
    : audio(engine, settings), sessionPanel(session, engine, tunings, settings), play(session, engine, settings, tunings)
{
    tabs.setWantsKeyboardFocus(false);
    tabs.getTabbedButtonBar().setWantsKeyboardFocus(false);
    const auto background = getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId);
    tabs.addTab("Session", background, &sessionPanel, false);
    tabs.addTab("Audio", background, &audio, false);
    tabs.setTabBarDepth(28);
    addAndMakeVisible(tabs);
    addAndMakeVisible(play);

    sessionPanel.onSelectedTrackChanged = [this] { play.setTrack(sessionPanel.selection().track); };
    play.setTrack(sessionPanel.selection().track);
}

void DebugUiPanel::resized()
{
    auto area = getLocalBounds().reduced(4);
    play.setBounds(area.removeFromBottom(playPanelHeight));
    area.removeFromBottom(8);
    tabs.setBounds(area);
}

// --- MainComponent -------------------------------------------------------------------------------------

MainComponent::MainComponent(AudioEngine& engine, juce::PropertiesFile& settings, TuningRegistry& tunings,
                             SessionController& session, std::function<void()> firstPaint)
    : onFirstPaint(std::move(firstPaint)), sessionController(session), debug(engine, settings, tunings, session)
{
    setOpaque(true);
    setWantsKeyboardFocus(true);

    auto& view = debug.sessionView();
    ui::ShellActions actions;
    actions.newSet = [&view] { view.newSet(); };
    actions.openSet = [&view] { view.openSet(); };
    actions.saveSet = [&view] { view.saveSet(); };
    actions.saveSetAs = [&view] { view.saveSetAs(); };
    actions.importMidi = [&view] { view.importMidi(view.selection().track, view.selection().scene); };
    mainShell = std::make_unique<ui::MainShell>(session, engine, settings, tunings, std::move(actions));
    mainShell->setDebugUi(&debug);
    mainShell->selection().addListener(this);
    view.onSelectionChanged = [this] { debugSelectionChanged(); };
    mainShell->controlBar().setComputerKeyboard(&debug.playPanel().computerKeyboard());
    addAndMakeVisible(*mainShell);

    setSize(1280, 960);
}

MainComponent::~MainComponent()
{
    debug.sessionView().onSelectionChanged = nullptr;
    mainShell->selection().removeListener(this);
    mainShell.reset(); // before the debug UI it shows
}

void MainComponent::selectionChanged(const ui::SessionSelection& s)
{
    if (!mirroring)
    {
        const juce::ScopedValueSetter<bool> guard(mirroring, true);
        // The debug view has no selected track header: a track selects its slot in the row.
        debug.sessionView().setSelection({s.track, s.scene, s.kind == ui::SessionSelection::Kind::scene});
    }
    // After the play panel has taken the track (it sets the keyboard's range for the track's
    // tuning): a Drum Rack's pads take over the range.
    keyboardFollow.trackSelected(sessionController.set(), s.track, debug.playPanel().computerKeyboard());
}

void MainComponent::debugSelectionChanged()
{
    if (mirroring)
        return;
    const juce::ScopedValueSetter<bool> guard(mirroring, true);
    const auto& d = debug.sessionView().selection();
    // The debug view re-announces its selection after the set changes (it clamps it): when it
    // still matches, the Session view's selection (a range, a track) stays as it is.
    const auto& now = mainShell->selection().get();
    if (d.track == now.track && d.scene == now.scene && d.sceneColumn == (now.kind == ui::SessionSelection::Kind::scene))
        return;
    mainShell->selection().set({d.sceneColumn ? ui::SessionSelection::Kind::scene : ui::SessionSelection::Kind::slot, d.track, d.scene});
}

void MainComponent::paint(juce::Graphics& g)
{
    g.fillAll(mainShell->theme().background);

    if (!firstPaintDone)
    {
        firstPaintDone = true;
        if (onFirstPaint)
            juce::MessageManager::callAsync(onFirstPaint);
    }
}

void MainComponent::resized()
{
    mainShell->setBounds(getLocalBounds());
}

bool MainComponent::keyPressed(const juce::KeyPress& key)
{
    // The shell's commands have had the key already (its key mappings); what's left is the
    // computer MIDI keyboard's.
    if (dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent()) != nullptr)
        return false; // typing in a text field

    const auto mods = key.getModifiers();
    if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown())
        return false;

    auto character = key.getTextCharacter();
    if (character == 0)
        character = static_cast<juce::juce_wchar>(key.getKeyCode());
    return debug.playPanel().computerKeyboard().keyDown(character, key.getKeyCode());
}

bool MainComponent::keyStateChanged(bool)
{
    auto& keyboard = debug.playPanel().computerKeyboard();
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
    setMenuBar(&component->shell().menus());
    setResizable(true, true);
    centreWithSize(getWidth(), getHeight());
    markStage("window laid out");
    setVisible(true);
    markStage("window visible");
    component->grabKeyboardFocus();
}

MainWindow::~MainWindow()
{
    setMenuBar(nullptr); // its model belongs to the content, which goes first
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
