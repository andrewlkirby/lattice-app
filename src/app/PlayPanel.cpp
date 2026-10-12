#include "app/PlayPanel.h"

namespace lattice
{

TuningKeyboardComponent::TuningKeyboardComponent(juce::MidiKeyboardState& keyboardState)
    : juce::MidiKeyboardComponent(keyboardState, juce::MidiKeyboardComponent::horizontalKeyboard)
{
    // Key presses are handled by ComputerKeyboardInput (Live's layout), not by this component.
    clearKeyMappings();
    setWantsKeyboardFocus(false);
    setAvailableRange(0, 127);
    setKeyWidth(22.0f);
    setScrollButtonsVisible(true);
}

void TuningKeyboardComponent::setTuning(TuningPtr t)
{
    tuning = std::move(t);
    repaint();
}

juce::String TuningKeyboardComponent::getWhiteNoteText(int midiNoteNumber)
{
    if (tuning == nullptr || tuning->usesNoteNames)
        return juce::MidiKeyboardComponent::getWhiteNoteText(midiNoteNumber);
    return juce::String(midiNoteNumber); // the table index; the readout shows its frequency
}

PlayPanel::PlayPanel(AudioEngine& e, juce::PropertiesFile& s, TuningRegistry& r)
    : engine(e), settings(s), registry(r),
      keyboardInput([this](const juce::MidiMessage& m) {
          // Through the on-screen keyboard's state, so computer-keyboard notes light up its keys;
          // the state's listener (below) forwards them to the engine.
          if (m.isNoteOn())
              keyboardState.noteOn(m.getChannel(), m.getNoteNumber(), m.getFloatVelocity());
          else if (m.isNoteOff())
              keyboardState.noteOff(m.getChannel(), m.getNoteNumber(), m.getFloatVelocity());
      })
{
    keyboardState.addListener(this);
    registry.addListener(this);

    addAndMakeVisible(tuningLabel);
    tuningBox.setWantsKeyboardFocus(false);
    tuningBox.onChange = [this] {
        const int index = tuningBox.getSelectedId() - 1;
        const auto& all = registry.getAll();
        if (index >= 0 && index < static_cast<int>(all.size()))
            selectTuning(all[static_cast<std::size_t>(index)]);
    };
    addAndMakeVisible(tuningBox);

    loadButton.setWantsKeyboardFocus(false);
    loadButton.onClick = [this] { chooseTuningFile(); };
    addAndMakeVisible(loadButton);

    keyboardToggle.setWantsKeyboardFocus(false);
    keyboardToggle.onClick = [this] { keyboardInput.setEnabled(keyboardToggle.getToggleState()); };
    addAndMakeVisible(keyboardToggle);
    addAndMakeVisible(keyboardInfo);

    lastNoteLabel.setFont(juce::FontOptions(18.0f, juce::Font::bold));
    addAndMakeVisible(lastNoteLabel);
    addAndMakeVisible(keyboard);

    keyboardInput.onStateChanged = [this] { updateKeyboardInfo(); };
    keyboardInput.setEnabled(settings.getBoolValue(computerKeyboardKey, true));

    // Tunings the user loaded before.
    for (const auto& path : juce::StringArray::fromLines(settings.getValue(userTuningFilesKey)))
        if (path.isNotEmpty())
            registry.loadFile(juce::File(path));

    rebuildTuningMenu();
    auto initial = registry.findById(settings.getValue(tuningIdKey));
    selectTuning(initial != nullptr ? initial : registry.defaultTuning());

    startTimerHz(15);
}

PlayPanel::~PlayPanel()
{
    keyboardInput.releaseAll();
    registry.removeListener(this);
    keyboardState.removeListener(this);
}

void PlayPanel::selectTuning(const TuningPtr& tuning)
{
    if (tuning == nullptr)
        return;

    keyboardInput.releaseAll();
    engine.trackTuning().setTuning(tuning);
    keyboardInput.configureForTuning(*tuning);
    keyboard.setTuning(tuning);
    keyboard.setLowestVisibleKey(keyboardInput.getBaseNote());
    settings.setValue(tuningIdKey, tuning->id);

    const auto& all = registry.getAll();
    for (std::size_t i = 0; i < all.size(); ++i)
        if (all[i] == tuning)
            tuningBox.setSelectedId(static_cast<int>(i) + 1, juce::dontSendNotification);

    shownLastNote = -2; // refresh the readout for the new tuning
    updateKeyboardInfo();
}

void PlayPanel::rebuildTuningMenu()
{
    tuningBox.clear(juce::dontSendNotification);
    const auto& all = registry.getAll();
    for (std::size_t i = 0; i < all.size(); ++i)
        tuningBox.addItem(all[i]->name, static_cast<int>(i) + 1);

    const auto current = engine.trackTuning().getTuning();
    for (std::size_t i = 0; i < all.size(); ++i)
        if (all[i] == current)
            tuningBox.setSelectedId(static_cast<int>(i) + 1, juce::dontSendNotification);
}

void PlayPanel::tuningsChanged()
{
    rebuildTuningMenu();
}

void PlayPanel::chooseTuningFile()
{
    chooser = std::make_unique<juce::FileChooser>("Load a tuning (.scl, or a Colundi-format .txt)",
                                                  juce::File::getSpecialLocation(juce::File::userDocumentsDirectory),
                                                  "*.scl;*.txt");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this](const juce::FileChooser& fc) {
                             const auto file = fc.getResult();
                             if (file == juce::File())
                                 return;

                             const auto result = registry.loadFile(file);
                             if (!result.ok())
                             {
                                 juce::AlertWindow::showMessageBoxAsync(
                                     juce::MessageBoxIconType::WarningIcon, "Could not load the tuning",
                                     file.getFileName() + (result.line > 0 ? " (line " + juce::String(result.line) + ")" : "")
                                         + ":\n" + result.error);
                                 return;
                             }

                             auto files = juce::StringArray::fromLines(settings.getValue(userTuningFilesKey));
                             files.removeEmptyStrings();
                             files.addIfNotAlreadyThere(file.getFullPathName());
                             settings.setValue(userTuningFilesKey, files.joinIntoString("\n"));
                             selectTuning(result.table);
                         });
}

void PlayPanel::updateKeyboardInfo()
{
    keyboardToggle.setToggleState(keyboardInput.isEnabled(), juce::dontSendNotification);
    settings.setValue(computerKeyboardKey, keyboardInput.isEnabled());

    const auto tuning = engine.trackTuning().getTuning();
    const int first = keyboardInput.getBaseNote();
    const int last = keyboardInput.getLastNote();
    juce::String range;
    if (tuning != nullptr && tuning->usesNoteNames)
        range = tuning->labels[static_cast<std::size_t>(first)] + " - " + tuning->labels[static_cast<std::size_t>(last)];
    else
        range = "#" + juce::String(first) + " - #" + juce::String(last);

    keyboardInfo.setText("A W S E D F T G Y H U J K O L P ;  plays " + range + "   |   Z/X: range (step "
                             + juce::String(keyboardInput.getPageSize()) + ")   |   C/V: velocity "
                             + juce::String(keyboardInput.getVelocity()),
                         juce::dontSendNotification);
    keyboardInfo.setEnabled(keyboardInput.isEnabled());
    keyboard.setLowestVisibleKey(first);
}

void PlayPanel::timerCallback()
{
    engine.trackTuning().collectGarbage();

    const int note = engine.synth().lastNoteOn();
    if (note == shownLastNote)
        return;
    shownLastNote = note;

    const auto tuning = engine.trackTuning().getTuning();
    if (note < 0 || tuning == nullptr)
    {
        lastNoteLabel.setText("Last note: -", juce::dontSendNotification);
        return;
    }

    const double hz = engine.trackTuning().hzFor(note);
    juce::String text = "Last note: #" + juce::String(note);
    if (tuning->usesNoteNames)
        text << " (" << tuning->labels[static_cast<std::size_t>(note)] << ")";
    text << "   " << juce::String(hz, 2) << " Hz";
    lastNoteLabel.setText(text, juce::dontSendNotification);
}

void PlayPanel::handleNoteOn(juce::MidiKeyboardState*, int channel, int note, float velocity)
{
    engine.midiInput().push(juce::MidiMessage::noteOn(channel, note, velocity));
}

void PlayPanel::handleNoteOff(juce::MidiKeyboardState*, int channel, int note, float velocity)
{
    engine.midiInput().push(juce::MidiMessage::noteOff(channel, note, velocity));
}

void PlayPanel::paint(juce::Graphics& g)
{
    g.setColour(getLookAndFeel().findColour(juce::ResizableWindow::backgroundColourId).brighter(0.05f));
    g.fillRoundedRectangle(getLocalBounds().toFloat(), 6.0f);
}

void PlayPanel::resized()
{
    auto area = getLocalBounds().reduced(10);

    auto row = area.removeFromTop(28);
    tuningLabel.setBounds(row.removeFromLeft(60));
    loadButton.setBounds(row.removeFromRight(150));
    row.removeFromRight(8);
    tuningBox.setBounds(row);
    area.removeFromTop(6);

    row = area.removeFromTop(26);
    keyboardToggle.setBounds(row.removeFromLeft(200));
    keyboardInfo.setBounds(row);
    area.removeFromTop(4);

    lastNoteLabel.setBounds(area.removeFromTop(28));
    area.removeFromTop(6);
    keyboard.setBounds(area);
}

} // namespace lattice
