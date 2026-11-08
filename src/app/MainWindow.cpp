#include "app/MainWindow.h"

#include "ui/prefs/AppSettings.h"

namespace lattice
{
// --- MainComponent -------------------------------------------------------------------------------------

MainComponent::MainComponent(AudioEngine& e, juce::PropertiesFile& settingsFile, TuningRegistry& registry,
                             SessionController& session, std::function<void()> firstPaint)
    : onFirstPaint(std::move(firstPaint)), engine(e), settings(settingsFile), tunings(registry), sessionController(session),
      keyboard([this](const juce::MidiMessage& m) { engine.midiInput().push(m); })
{
    setOpaque(true);
    setWantsKeyboardFocus(true);
    ui::prefs::applyTestTone(settings, engine);

    keyboard.setEnabled(settings.getBoolValue(ui::prefs::computerKeyboardKey, true));
    keyboard.onStateChanged = [this] { settings.setValue(ui::prefs::computerKeyboardKey, keyboard.isEnabled()); };

    mainShell = std::make_unique<ui::MainShell>(session, engine, settings, tunings, ui::ShellActions{});
    mainShell->startControlSurfaces(ui::MidiPorts::system(engine));
    mainShell->setComputerKeyboard(&keyboard); // its control bar shows the range (and keeps onStateChanged)
    mainShell->tuningMaster().setEnabled(settings.getBoolValue(ui::prefs::mtsEspMasterKey, true));
    mainShell->onKeyboardSettingsChanged = [this] { followSelectedTrack(true); };
    mainShell->mappings().isKeyboardKey = [this](const juce::KeyPress& key) {
        if (!keyboard.isEnabled() || key.getModifiers().isAnyModifierKeyDown())
            return false;
        const auto c = juce::CharacterFunctions::toLowerCase(key.getTextCharacter());
        return c > 0 && (keyboard.getNoteKeys().containsChar(c) || juce::String("zxcvm").containsChar(c));
    };
    mainShell->selection().addListener(this);
    session.set().state().addListener(this);
    tunings.addListener(this);
    addAndMakeVisible(*mainShell);
    followSelectedTrack(true);

    setSize(1280, 960);
}

MainComponent::~MainComponent()
{
    keyboard.releaseAll();
    if (deviceOpen)
    {
        engine.getDeviceManager().removeChangeListener(this);
        if (persistDeviceState)
            ui::prefs::saveDeviceState(settings, engine);
    }
    tunings.removeListener(this);
    sessionController.set().state().removeListener(this);
    mainShell->selection().removeListener(this);
    mainShell.reset();
    keyboard.onStateChanged = nullptr; // the control bar's, gone with the shell
}

void MainComponent::audioDeviceOpened(const juce::String& error)
{
    // The device manager was the opening thread's until now.
    deviceOpen = true;
    engine.getDeviceManager().addChangeListener(this);
    mainShell->audioDeviceOpened(error);
}

void MainComponent::changeListenerCallback(juce::ChangeBroadcaster*)
{
    if (persistDeviceState)
        ui::prefs::saveDeviceState(settings, engine);
}

void MainComponent::tuningsChanged()
{
    sessionController.snapshots().invalidate(); // tracks may name the tuning that just arrived
    followSelectedTrack(true);
}

void MainComponent::followSelectedTrack(bool retune)
{
    auto& set = sessionController.set();
    const int track = juce::jlimit(0, juce::jmax(0, set.numTracks() - 1), mainShell->selection().get().track);
    auto tuning = tunings.findById(set.track(track)[ids::tuningId].toString());
    if (tuning == nullptr)
        tuning = tunings.defaultTuning();
    if (tuning != nullptr && (retune || tuning != keyboardTuning))
    {
        keyboardTuning = tuning;
        keyboard.releaseAll();
        keyboard.configureForTuning(*tuning);
    }
    // Then a Drum Rack's pads take over the range.
    keyboardFollow.trackSelected(set, track, keyboard);
}

void MainComponent::selectionChanged(const ui::SessionSelection&)
{
    followSelectedTrack(false);
}

void MainComponent::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    if (property != ids::padStart && property != ids::tuningId)
        return;
    const auto& s = mainShell->selection().get();
    auto& set = sessionController.set();
    if (s.track >= 0 && s.track < set.numTracks() && LiveSet::chainOwnerOf(tree) == set.track(s.track))
        followSelectedTrack(false);
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
    // With the focus inside the shell, its commands have had the key already (its key mappings).
    // Keys only travel up from the focused component, so with the focus here (at startup, or
    // after a dialog closes) the commands get it first; what's left is the computer MIDI keyboard's.
    if (!mainShell->hasKeyboardFocus(true) && mainShell->commands().keyMappings().keyPressed(key, mainShell.get()))
        return true;
    if (dynamic_cast<juce::TextEditor*>(juce::Component::getCurrentlyFocusedComponent()) != nullptr)
        return false; // typing in a text field
    if (mainShell->mappings().handleKey(key))
        return true; // Key Map (it leaves the computer keyboard's keys to it while it's on)

    const auto mods = key.getModifiers();
    if (mods.isCommandDown() || mods.isCtrlDown() || mods.isAltDown())
        return false;

    auto character = key.getTextCharacter();
    if (character == 0)
        character = static_cast<juce::juce_wchar>(key.getKeyCode());
    return keyboard.keyDown(character, key.getKeyCode());
}

bool MainComponent::keyStateChanged(bool)
{
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
    // The title names the set, with a * while it has unsaved changes.
    setName(component->shell().files().title());
    component->shell().files().onTitleChanged = [this] { setName(component->shell().files().title()); };
    // The window itself can have the focus (when a dialog closes, Windows may give it back here):
    // its keys still reach the commands.
    addKeyListener(&component->shell().commands().keyMappings());
    setResizable(true, true);
    centreWithSize(getWidth(), getHeight());
    markStage("window laid out");
    setVisible(true);
    markStage("window visible");
    component->grabKeyboardFocus();
}

MainWindow::~MainWindow()
{
    component->shell().files().onTitleChanged = nullptr;
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
