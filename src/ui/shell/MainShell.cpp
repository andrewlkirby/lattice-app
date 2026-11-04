#include "ui/shell/MainShell.h"

#include "core/model/UndoNames.h"

#include "platform/Platform.h"
#include "ui/prefs/AppSettings.h"
#include "ui/widgets/Accessible.h"
#include "ui/clip/ClipCommands.h"
#include "ui/device/DeviceCommands.h"

namespace lattice::ui
{

class MainShell::Placeholder final : public juce::Component
{
public:
    Placeholder(juce::String heading, juce::String text) : title(std::move(heading)), message(std::move(text))
    {
        setTitle(title);
        setHelpText(message);
    }

    void paint(juce::Graphics& g) override
    {
        const auto& t = themeOf(*this);
        g.fillAll(t.surface);
        g.setColour(t.border);
        g.drawRect(getLocalBounds());
        auto area = getLocalBounds().reduced(juce::roundToInt(t.padding * 2.0f));
        g.setColour(t.textDim);
        g.setFont(juce::Font(t.font(t.fontNormal)).boldened());
        g.drawFittedText(title, area.removeFromTop(juce::roundToInt(t.fontNormal + 6.0f)), juce::Justification::topLeft, 1);
        g.setFont(t.font(t.fontSmall));
        g.drawFittedText(message, area, juce::Justification::topLeft, 4);
    }

private:
    juce::String title, message;
};

/** Shift+Tab in a control reached with Tab (a knob, a button, a text field) goes back to the one
    before; in a view (accessible::markAsView), and with nothing focused, it's the Clip/Device
    switch. */
class MainShell::ShiftTabBack final : public juce::KeyListener
{
public:
    bool keyPressed(const juce::KeyPress& key, juce::Component*) override
    {
        if (key != juce::KeyPress(juce::KeyPress::tabKey, juce::ModifierKeys::shiftModifier, 0))
            return false;
        auto* focused = juce::Component::getCurrentlyFocusedComponent();
        if (focused == nullptr || accessible::isView(focused) || dynamic_cast<MainShell*>(focused) != nullptr)
            return false;
        focused->moveKeyboardFocusToSibling(false);
        return true;
    }
};

MainShell::MainShell(SessionController& s, AudioEngine& e, juce::PropertiesFile& settingsFile, TuningRegistry& tunings,
                     ShellActions actions)
    : session(s), engine(e), settings(settingsFile), registry(&s.undoManager()), stateFeed(s, e), deviceSpecs(s.deviceRegistry())
{
    setLookAndFeel(&lookAndFeel);
    if (const auto* saved = Theme::builtIn(settings.getValue(themeKey, "Dark")))
        lookAndFeel.setTheme(*saved);
    // A theme from the Themes folder, if one was chosen.
    if (const juce::File userTheme(settings.getValue(prefs::userThemeKey)); juce::File::isAbsolutePath(userTheme.getFullPathName()) && userTheme.existsAsFile())
    {
        juce::StringArray problems;
        auto theme = Theme::fromJson(juce::JSON::parse(userTheme), problems);
        if (theme.name.isEmpty())
            theme.name = userTheme.getFileNameWithoutExtension();
        lookAndFeel.setTheme(theme);
    }

    if (!actions.rename)
        actions.rename = [this] {
            if (session_ != nullptr)
                session_->renameSelection();
        };
    if (!actions.importMidi)
        actions.importMidi = [this] {
            // Into the selected slot (the command is on only with a slot selected).
            const auto at = selected.get();
            if (at.kind != SessionSelection::Kind::slot || deviceShared == nullptr || !deviceShared->chooseFile)
                return;
            deviceShared->chooseFile("Import a MIDI File", "*.mid;*.midi", [this, track = at.track, scene = at.scene](const juce::File& file) {
                const auto result = session.importMidiFile(file, track, scene);
                if (!result.ok() && deviceShared->confirm)
                    deviceShared->confirm("Can't import " + file.getFileName(), result.error, {}, nullptr);
            });
        };
    // The set's files (4g): New, Open, Save and Save As are the shell's own.
    SetFileActions::Dialogs fileDialogs;
    fileDialogs.ask = [this](const juce::String& title, const juce::String& message, const juce::StringArray& buttons,
                             std::function<void(int)> answer) {
        auto options = juce::MessageBoxOptions().withTitle(title).withMessage(message).withIconType(juce::MessageBoxIconType::QuestionIcon)
                           .withAssociatedComponent(this);
        for (const auto& b : buttons)
            options = options.withButton(b);
        // JUCE numbers the buttons 1, 2... with the last one 0; as indices: 0, 1... (-1: none).
        const int count = buttons.size();
        juce::AlertWindow::showAsync(options, [answer = std::move(answer), count, refocus = [this, before = focusToRestore()] {
                                                   restoreFocusLater(before);
                                               }](int result) {
            refocus();
            if (answer)
                answer(result == 0 ? count - 1 : result - 1);
        });
    };
    fileDialogs.chooseFile = [this](const juce::String& title, const juce::File& start, const juce::String& patterns, bool save,
                                    std::function<void(const juce::File&)> chosen) {
        chooser = std::make_unique<juce::FileChooser>(title, start, patterns);
        const int mode = save ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                                     juce::FileBrowserComponent::warnAboutOverwriting
                               : juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
        chooser->launchAsync(mode, [this, chosen = std::move(chosen), before = focusToRestore()](const juce::FileChooser& fc) {
            restoreFocusLater(before);
            chosen(fc.getResult());
        });
    };
    fileDialogs.tell = [this](const juce::String& title, const juce::String& message) {
        juce::AlertWindow::showAsync(juce::MessageBoxOptions().withTitle(title).withMessage(message).withButton("OK").withAssociatedComponent(this),
                                     [this, before = focusToRestore()](int) { restoreFocusLater(before); });
    };
    setFiles = std::make_unique<SetFileActions>(session, settings, jobPool, platform::userDataFolder().getChildFile("Recovery"),
                                                std::move(fileDialogs));
    setFiles->onMissingFiles = [this](std::vector<setfiles::Reference> missing) { showMissingFiles(std::move(missing)); };
    actions.newSet = [this] { setFiles->newSet(); };
    actions.openSet = [this] { setFiles->open(); };
    actions.saveSet = [this] { setFiles->save(); };
    actions.saveSetAs = [this] { setFiles->saveAs(); };
    addSessionCommands(registry, session,
                       {[this] { return selected.get(); }, [this](SessionSelection sel) { selected.set(sel); },
                        [this]() -> EditTarget* {
                            if (auto* t = clip != nullptr ? clip->editTargetIfFocused() : nullptr)
                                return t;
                            if (auto* t = tunings_ != nullptr ? tunings_->editTargetIfFocused() : nullptr)
                                return t;
                            return devices != nullptr ? devices->editTargetIfFocused() : nullptr;
                        }},
                       std::move(actions));
    addViewCommands();
    addFileCommands();
    // The Preferences (4g): shortcuts as saved, and saved whenever they change.
    prefs::restoreShortcuts(settings, registry);
    registry.keyMappings().addChangeListener(this);
    addKeyListener(&registry.keyMappings());
    shiftTabBack = std::make_unique<ShiftTabBack>();
    addKeyListener(shiftTabBack.get()); // asked before the key mappings
    menuModel = std::make_unique<CommandMenus>(registry);
    menuModel->addSubmenuAfter("file.open", "Open Recent", [this] { return setFiles->recentMenu(); });

    bar = std::make_unique<ControlBar>(session, registry, stateFeed);
    bar->frameSummary = [this] { return frames.summary(); };
    bar->frameBudgetSeconds = frameBudgetSeconds;
    addAndMakeVisible(*bar);

    clip = std::make_unique<ClipView>(session, stateFeed, tunings, selected);
    presetLibrary = std::make_unique<presets::PresetLibrary>(presets::PresetLibrary::defaultUserRoot(platform::userDataFolder()),
                                                             session.deviceRegistry());
    libraryService = std::make_unique<library::Library>(platform::userDataFolder(),
                                                         library::Library::defaultDatabaseFile(platform::userDataFolder()),
                                                         session.deviceRegistry(), *presetLibrary, tunings, jobPool);
    deviceShared = std::make_unique<DeviceContext>(DeviceContext{.session = session,
                                                                 .feed = stateFeed,
                                                                 .specs = deviceSpecs,
                                                                 .presets = *presetLibrary,
                                                                 .tunings = tunings,
                                                                 .jobs = jobPool});
    deviceShared->chooseFile = [this](const juce::String& title, const juce::String& patterns,
                                      std::function<void(const juce::File&)> chosen) {
        chooser = std::make_unique<juce::FileChooser>(title, juce::File::getSpecialLocation(juce::File::userMusicDirectory), patterns);
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [this, chosen = std::move(chosen), before = focusToRestore()](const juce::FileChooser& fc) {
                                 if (fc.getResult().existsAsFile())
                                     chosen(fc.getResult());
                                 restoreFocusLater(before);
                             });
    };
    deviceShared->confirm = [this](const juce::String& title, const juce::String& question, const juce::String& yesText,
                                   std::function<void()> yes) {
        auto options = juce::MessageBoxOptions().withTitle(title).withMessage(question).withAssociatedComponent(this);
        auto refocus = [this, before = focusToRestore()] { restoreFocusLater(before); };
        if (yesText.isEmpty())
        {
            juce::AlertWindow::showAsync(options.withIconType(juce::MessageBoxIconType::InfoIcon).withButton("OK"),
                                         [refocus](int) { refocus(); });
            return;
        }
        juce::AlertWindow::showAsync(options.withIconType(juce::MessageBoxIconType::QuestionIcon).withButton(yesText).withButton("Cancel"),
                                     [yes = std::move(yes), refocus](int result) {
                                         if (result == 1 && yes)
                                             yes();
                                         refocus();
                                     });
    };
    deviceShared->peaks = &libraryService->peaks();
    devices = std::make_unique<DeviceView>(*deviceShared, selected);
    TuningView::Actions tuningActions;
    tuningActions.sampleRate = [this] {
        auto* device = engine.getDeviceManager().getCurrentAudioDevice();
        return device != nullptr ? device->getCurrentSampleRate() : 48000.0;
    };
    // Through the device context's, whatever it is at the time (tests answer it themselves).
    tuningActions.confirm = [this](const juce::String& title, const juce::String& message, const juce::String& yes,
                                   std::function<void()> then) {
        if (deviceShared->confirm)
            deviceShared->confirm(title, message, yes, std::move(then));
    };
    tuningActions.ask = [this](const juce::String& title, const juce::String& message, const juce::String& initial,
                               std::function<void(const juce::String&)> answer) { askText(title, message, initial, std::move(answer)); };
    tuningActions.loadFile = [this](int track) { chooseTuningFile(track); };
    tuningActions.userTunings = userTuningsFolder();
    tunings_ = std::make_unique<TuningView>(session, tunings, selected, std::move(tuningActions));
    detail = std::make_unique<DetailView>(*clip, *devices, *tunings_);
    devices->onTitleChanged = [this](const juce::String& title) { detail->setDeviceTitle(title); };
    detail->setDeviceTitle(devices->ownerName());
    devices->onShowRequested = [this] { showDeviceView(); };
    addDeviceCommands(registry, *devices);
    addClipCommands(registry, clip->pianoRoll());
    clip->pianoRoll().setCommands(&registry);
    clip->pianoRoll().onHover = [this](const juce::String&) { showInfoFor(&clip->pianoRoll()); };
    session_ = std::make_unique<SessionView>(session, stateFeed, registry, selected, tunings);
    session_->setMixerVisible(settings.getBoolValue(mixerKey, true));
    session_->setSendsVisible(settings.getBoolValue(sendsKey, true));
    session_->setIOVisible(settings.getBoolValue(ioKey, true));
    session_->onOpenClip = [this] { showClipView(); };
    session_->onShowDevices = [this] {
        // As in Live: a track's header shows its devices, if the detail view is open.
        if (showDetail)
            detail->show(DetailView::Page::device);
    };
    session_->onBrowserDrop = [this](const SessionView::Hit& hit, const juce::String& description) {
        return dropFromBrowser(hit, description);
    };
    // MIDI Map and Key Map (4g).
    mapper = std::make_unique<Mappings>(session, registry, deviceSpecs);
    mapper->pullMidi = [this] {
        std::vector<std::pair<juce::String, juce::MidiMessage>> out;
        for (auto& m : engine.takeRemoteMidi())
            out.emplace_back(std::move(m.input), std::move(m.message));
        return out;
    };
    // MIDI Map ticks with the display while it has something to do; MIDI coming in (the engine's
    // activity) starts the ticks again.
    mapperWake.onWake = [this] {
        mapperIdleFrames = 0;
        if (mapperTick == nullptr)
            mapperTick = std::make_unique<juce::VBlankAttachment>(this, [this](double) { tickMapper(); });
    };
    engine.addActivityListener(&mapperWake);
    mapperWake.onWake();
    overlay = std::make_unique<MapOverlay>(*mapper, controls, *session_);
    overlay->onExit = [this] { setMapMode(Mappings::Mode::off); };
    overlay->passesThrough = [this](const juce::KeyPress& key) {
        for (const auto* id : {"view.midiMap", "view.keyMap"})
            if (registry.shortcutsFor(id).contains(key))
                return true;
        return false;
    };
    addChildComponent(*overlay);
    mappingList = std::make_unique<MappingsView>(*mapper, registry);
    addChildComponent(*mappingList);
    mapper->onChange = [this] {
        mappingList->refresh();
        overlay->repaint();
    };
    addMappingCommands();
    applyMidiInputs();

    // Find (4g).
    finder = std::make_unique<FindIndex>(*deviceShared, registry);
    finder->extra = [this](const juce::StringArray& words, const juce::String& typed) {
        // The selected clip's settings: the Clip view's controls (they exist while a clip is selected).
        std::vector<FindIndex::Result> out;
        for (const auto* entry : controls.entries())
        {
            if (entry->component == nullptr || !clip->isParentOf(entry->component) || entry->binding == nullptr)
                continue;
            if (!FindIndex::matches(words, entry->name + " " + entry->group + " Clip"))
                continue;
            FindIndex::Result r;
            r.kind = FindIndex::Kind::clip;
            r.name = entry->name;
            r.location = juce::String("Clip") + juce::String::fromUTF8(" \xe2\x80\xba ") + (entry->group.isNotEmpty() ? entry->group : juce::String("Properties"));
            r.section = 0;
            r.score = FindIndex::scoreFor(entry->name, typed);
            r.control = entry->component;
            out.push_back(std::move(r));
        }
        return out;
    };
    findBox = std::make_unique<FindView>(*finder);
    findBox->selectedTrack = [this] {
        const auto now = selected.get();
        return now.kind == SessionSelection::Kind::scene ? -1 : now.track;
    };
    findBox->onChoose = [this](const FindIndex::Result& r) { goTo(r); };
    findBox->onClose = [this, before = juce::Component::SafePointer<juce::Component>()] { repaint(); };
    addChildComponent(*findBox);
    addFindCommands();
    session_->onLoadTuning = [this](int track) { chooseTuningFile(track); };
    session_->onShowTuning = [this] { showTuningView(); };
    addTuningCommands();
    session.onSetReplaced = [this, before = session.onSetReplaced] {
        tunings_->setReplaced();
        if (before)
            before();
    };

    BrowserView::Actions browsing;
    browsing.insert = [this](const juce::String& description) { return insertFromBrowser(description); };
    browsing.chooseFolder = [this](std::function<void(const juce::File&)> chosen) {
        chooser = std::make_unique<juce::FileChooser>("Add a folder to Places", juce::File::getSpecialLocation(juce::File::userMusicDirectory));
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                             [this, chosen = std::move(chosen), before = focusToRestore()](const juce::FileChooser& fc) {
                                 if (fc.getResult().isDirectory())
                                     chosen(fc.getResult());
                                 restoreFocusLater(before);
                             });
    };
    browsing.sampleRate = [this] {
        auto* device = engine.getDeviceManager().getCurrentAudioDevice();
        return device != nullptr ? device->getCurrentSampleRate() : 48000.0;
    };
    browser = std::make_unique<BrowserView>(*libraryService, jobPool, engine, stateFeed, *presetLibrary, &settings, std::move(browsing));
    arrangementPlaceholder = std::make_unique<Placeholder>("Arrangement", "After the MVP.");
    addChildComponent(*browser);
    addChildComponent(*detail);

    tabs.setWantsKeyboardFocus(false);
    tabs.getTabbedButtonBar().setWantsKeyboardFocus(false);
    tabs.setOutline(0);
    tabs.addTab("Session", lookAndFeel.theme().background, session_.get(), false);
    tabs.addTab("Arrangement", lookAndFeel.theme().background, arrangementPlaceholder.get(), false);
    if (auto* arrangement = tabs.getTabbedButtonBar().getTabButton(1))
        arrangement->setEnabled(false);
    addAndMakeVisible(tabs);

    info.setTitle("Info view");
    addAndMakeVisible(info);
    history = std::make_unique<UndoHistoryView>(session.undoManager());
    addChildComponent(*history);
    status = std::make_unique<StatusBar>(session, jobPool);
    addAndMakeVisible(*status);
    jobPool.onJobsChanged = [this] { status->refresh(); };
    session.set().state().addListener(this);

    showBrowser = settings.getBoolValue(browserKey, false);
    showDetail = settings.getBoolValue(detailKey, false);
    detailHeight = settings.getIntValue(detailHeightKey, 0);
    browserWidth = settings.getIntValue(browserWidthKey, 0);
    showInfo = settings.getBoolValue(infoKey, true);
    showHistory = settings.getBoolValue(historyKey, false);
    history->setVisible(showHistory);
    browser->setVisible(showBrowser);
    detail->setVisible(showDetail);
    info.setVisible(showInfo);

    autoArm = settings.getBoolValue(autoArmKey, true);
    selected.addListener(this);
    selectionChanged(selected.get());

    addControllerCommands();
    pushDocked = !settings.getBoolValue(virtualPushWindowKey, false);
    pushWidth = settings.getIntValue(virtualPushWidthKey, 0);
    if (settings.getBoolValue(virtualPushKey, false))
        setVirtualPushVisible(true);

    stateFeed.attachTo(*this);
    // Every mouse event in the window, for the Info view and the dividers (not a global listener:
    // JUCE polls the mouse ten times a second for those, idle or not).
    addMouseListener(this, true);
}

MainShell::~MainShell()
{
    session.onSetReplaced = nullptr;
    virtualSurface.reset();
    virtualPushWindow.reset();
    virtualPush.reset();
    preferencesWindow.reset();
    findBox.reset();
    finder.reset();
    overlay.reset();
    mappingList.reset();
    mapper.reset();
    registry.keyMappings().removeChangeListener(this);
    if (missingWindow != nullptr)
        delete missingWindow.getComponent();
    setFiles.reset(); // it waits for an autosave being written
    browser.reset(); // it plays through the engine and reads the library
    selected.removeListener(this);
    removeMouseListener(this);
    engine.removeActivityListener(&mapperWake);
    mapperWake.cancelPendingUpdate();
    mapperTick.reset();
    session.set().state().removeListener(this);
    stateFeed.detach();
    removeKeyListener(shiftTabBack.get());
    removeKeyListener(&registry.keyMappings());
    tabs.clearTabs();
    session_.reset();
    bar.reset();
    menuModel.reset();
    setLookAndFeel(nullptr);
}

void MainShell::addViewCommands()
{
    using juce::KeyPress;
    using juce::ModifierKeys;
    const auto ctrlAlt = [](char c) { return KeyPress(c, ModifierKeys::commandModifier | ModifierKeys::altModifier, 0); };
    registry.add({.id = "view.browser",
                  .name = "Show Browser",
                  .category = "View",
                  .description = "Shows or hides the browser.",
                  .defaultKeys = {ctrlAlt('b')},
                  .perform = [this] { setBrowserVisible(!showBrowser); },
                  .isTicked = [this] { return showBrowser; }});
    registry.add({.id = "view.searchBrowser",
                  .name = "Search in Browser",
                  .category = "View",
                  .description = "Shows the browser and puts the keys in its search box (Find is Ctrl+F).",
                  .defaultKeys = {ctrlAlt('f')},
                  .perform =
                      [this] {
                          setBrowserVisible(true);
                          browser->focusSearch();
                      }});
    registry.add({.id = "view.preview",
                  .name = "Preview in Browser",
                  .category = "View",
                  .description = "Selecting a sample in the browser plays it, and an instrument preset plays a phrase.",
                  .perform = [this] { browser->setPreviewOn(!browser->isPreviewOn()); },
                  .isTicked = [this] { return browser != nullptr && browser->isPreviewOn(); }});
    registry.add({.id = "view.previewSync",
                  .name = "Sync Preview to the Bar",
                  .category = "View",
                  .description = "While the set plays, a preview starts on the next bar.",
                  .perform = [this] { browser->setPreviewSynced(!browser->isPreviewSynced()); },
                  .isTicked = [this] { return browser != nullptr && browser->isPreviewSynced(); }});
    registry.add({.id = "view.addPlace",
                  .name = "Add Folder to Places...",
                  .category = "View",
                  .description = "Adds a folder to the browser's places: its samples, presets and tunings are indexed and watched.",
                  .perform =
                      [this] {
                          setBrowserVisible(true);
                          browser->addPlace();
                      }});
    registry.add({.id = "view.rescanLibrary",
                  .name = "Rescan the Library",
                  .category = "View",
                  .description = "Reads every place in the browser again (new files appear by themselves; this is for when they didn't).",
                  .perform = [this] { libraryService->rescanAll(); }});
    registry.add({.id = "view.detail",
                  .name = "Show Detail View",
                  .category = "View",
                  .description = "Shows or hides the detail view (the Clip and Device views).",
                  .defaultKeys = {ctrlAlt('l')},
                  .perform = [this] { setDetailVisible(!showDetail); },
                  .isTicked = [this] { return showDetail; }});
    registry.add({.id = "view.detailPage",
                  .name = "Clip/Device View",
                  .category = "View",
                  .description = "Switches the detail view between the Clip view and the Device view (and shows it).",
                  .defaultKeys = {KeyPress(KeyPress::tabKey, ModifierKeys::shiftModifier, 0)},
                  .perform =
                      [this] {
                          if (!showDetail)
                              setDetailVisible(true);
                          else
                              detail->toggle();
                      }});
    registry.add({.id = "view.nextArea",
                  .name = "Next Area",
                  .category = "View",
                  .description = "Moves the keys to the next part of the window: the control bar, the browser, the Session grid, the mixer, "
                                 "the detail view, the undo history.",
                  .defaultKeys = {KeyPress(KeyPress::F6Key)},
                  .perform = [this] { moveFocusToArea(1); }});
    registry.add({.id = "view.previousArea",
                  .name = "Previous Area",
                  .category = "View",
                  .description = "Moves the keys to the previous part of the window.",
                  .defaultKeys = {KeyPress(KeyPress::F6Key, ModifierKeys::shiftModifier, 0)},
                  .perform = [this] { moveFocusToArea(-1); }});
    registry.add({.id = "view.info",
                  .name = "Show Info View",
                  .category = "View",
                  .description = "Shows or hides the info view, which describes what's under the pointer.",
                  .defaultKeys = {KeyPress('/', ModifierKeys::shiftModifier, '?')}, // Shift+?, as in Live
                  .perform = [this] { setInfoVisible(!showInfo); },
                  .isTicked = [this] { return showInfo; }});
    registry.add({.id = "session.autoArm",
                  .name = "Auto-Arm",
                  .category = "Session",
                  .description = "Arms the selected MIDI track while no track is armed by hand, so the keyboards play it.",
                  .perform = [this] { setAutoArm(!autoArm); },
                  .isTicked = [this] { return autoArm; }});
    registry.add({.id = "view.mixer",
                  .name = "Show Mixer",
                  .category = "View",
                  .description = "Shows or hides the Session view's mixer.",
                  .defaultKeys = {ctrlAlt('m')},
                  .perform =
                      [this] {
                          session_->setMixerVisible(!session_->isMixerVisible());
                          settings.setValue(mixerKey, session_->isMixerVisible());
                      },
                  .isTicked = [this] { return session_ != nullptr && session_->isMixerVisible(); }});
    registry.add({.id = "view.io",
                  .name = "Show In/Out",
                  .category = "View",
                  .description = "Shows or hides the tracks' MIDI input, channel and monitoring in the Session view's mixer.",
                  .defaultKeys = {ctrlAlt('i')},
                  .perform =
                      [this] {
                          session_->setIOVisible(!session_->isIOVisible());
                          settings.setValue(ioKey, session_->isIOVisible());
                      },
                  .isTicked = [this] { return session_ != nullptr && session_->isIOVisible(); }});
    registry.add({.id = "view.sends",
                  .name = "Show Sends",
                  .category = "View",
                  .description = "Shows or hides the sends in the Session view's mixer.",
                  .defaultKeys = {ctrlAlt('s')},
                  .perform =
                      [this] {
                          session_->setSendsVisible(!session_->areSendsVisible());
                          settings.setValue(sendsKey, session_->areSendsVisible());
                      },
                  .isTicked = [this] { return session_ != nullptr && session_->areSendsVisible(); }});
    registry.add({.id = "view.resetLoad",
                  .name = "Reset Load Statistics",
                  .category = "View",
                  .description = "Starts the audio load and UI frame time figures over.",
                  .perform = [this] { resetLoadStatistics(); }});
    for (const auto* t : {&Theme::dark(), &Theme::light(), &Theme::highContrast()})
    {
        const auto themeName = t->name;
        registry.add({.id = "view.theme" + themeName.removeCharacters(" "),
                      .name = themeName + " Theme",
                      .category = "View",
                      .description = "Switches the UI to the " + themeName + " theme.",
                      .perform = [this, t] { setTheme(*t); },
                      .isTicked = [this, themeName] { return theme().name == themeName; }});
    }
}

void MainShell::addFileCommands()
{
    registry.add({.id = "app.preferences",
                  .name = "Preferences...",
                  .category = "File",
                  .description = "Audio and MIDI devices, the look, the library's folders, CPU, tuning defaults, the computer keyboard and shortcuts.",
                  .defaultKeys = {juce::KeyPress(',', juce::ModifierKeys::commandModifier, 0)},
                  .perform = [this] { showPreferences(); }});
    registry.add({.id = "file.collectAll",
                  .name = "Collect All and Save",
                  .category = "File",
                  .description = "Copies every sample the set uses into a Samples folder beside it, then saves, so the folder holds everything.",
                  .perform = [this] { setFiles->collectAllAndSave(); }});
    registry.add({.id = "file.findMissing",
                  .name = "Find Missing Files...",
                  .category = "File",
                  .description = "Lists the samples the set uses that aren't where they were, and finds them in the browser's places.",
                  .perform =
                      [this] {
                          auto missing = setfiles::missing(session.set().state());
                          if (missing.empty())
                          {
                              if (deviceShared->confirm)
                                  deviceShared->confirm("Find Missing Files", "Every file the set uses is where it should be.", {}, nullptr);
                              return;
                          }
                          showMissingFiles(std::move(missing));
                      }});
    registry.add({.id = "view.undoHistory",
                  .name = "Show Undo History",
                  .category = "View",
                  .description = "Shows or hides the list of changes under the browser; click one to go back to it.",
                  .defaultKeys = {juce::KeyPress('h', juce::ModifierKeys::commandModifier | juce::ModifierKeys::altModifier, 0)},
                  .perform = [this] { setUndoHistoryVisible(!showHistory); },
                  .isTicked = [this] { return showHistory; }});
}

void MainShell::addMappingCommands()
{
    using juce::KeyPress;
    using juce::ModifierKeys;
    registry.add({.id = "view.midiMap",
                  .name = "MIDI Map Mode",
                  .category = "View",
                  .description = "Map controls, clip slots, scenes and commands to MIDI notes and CCs: click one, then move a knob or press a pad.",
                  .defaultKeys = {KeyPress('m', ModifierKeys::commandModifier, 0)},
                  .perform = [this] { setMapMode(mapper->mode() == Mappings::Mode::midi ? Mappings::Mode::off : Mappings::Mode::midi); },
                  .isTicked = [this] { return mapper != nullptr && mapper->mode() == Mappings::Mode::midi; }});
    registry.add({.id = "view.keyMap",
                  .name = "Key Map Mode",
                  .category = "View",
                  .description = "Map controls, clip slots, scenes and commands to computer keys: click one, then press a key.",
                  .defaultKeys = {KeyPress('k', ModifierKeys::commandModifier, 0)},
                  .perform = [this] { setMapMode(mapper->mode() == Mappings::Mode::key ? Mappings::Mode::off : Mappings::Mode::key); },
                  .isTicked = [this] { return mapper != nullptr && mapper->mode() == Mappings::Mode::key; }});
}

void MainShell::addFindCommands()
{
    using juce::KeyPress;
    using juce::ModifierKeys;
    registry.add({.id = "view.find",
                  .name = "Find...",
                  .category = "View",
                  .description = "Finds any control in the set, any setting or any command by name, and takes you to it.",
                  .defaultKeys = {KeyPress('f', ModifierKeys::commandModifier, 0)},
                  .perform = [this] { openFind(false); }});
    registry.add({.id = "view.commandPalette",
                  .name = "Command Palette...",
                  .category = "View",
                  .description = "Runs any command by typing part of its name.",
                  .defaultKeys = {KeyPress('p', ModifierKeys::commandModifier | ModifierKeys::shiftModifier, 0)},
                  .perform = [this] { openFind(true); }});
}

void MainShell::openFind(bool commandsOnly)
{
    const auto& t = theme();
    const int width = juce::jmin(getWidth() - juce::roundToInt(t.padding * 4.0f), juce::roundToInt(t.controlHeight * 30.0f));
    findBox->setBounds((getWidth() - width) / 2, bar->getBottom() + juce::roundToInt(t.spacing * 2.0f), width, findBox->preferredHeight());
    findBox->open(commandsOnly);
}

void MainShell::goTo(const FindIndex::Result& r)
{
    switch (r.kind)
    {
    case FindIndex::Kind::parameter:
        devices->revealParameter(r.device, r.parameterId); // it shows the Device view and flashes the control
        break;
    case FindIndex::Kind::mixer:
        revealMixerControl(r.owner, r.parameterId);
        break;
    case FindIndex::Kind::clip:
        showClipView();
        flash(r.control.getComponent());
        break;
    case FindIndex::Kind::setting:
        showPreferences(static_cast<PreferencesView::Page>(juce::jlimit(0, PreferencesView::numPages - 1, r.page)));
        break;
    case FindIndex::Kind::command:
        registry.perform(r.commandId);
        break;
    }
}

void MainShell::revealMixerControl(const juce::ValueTree& owner, const juce::String& property)
{
    auto& set = session.set();
    // Its track selected (a return's or the master's devices shown), the mixer and its section on.
    int column = -1;
    const auto& columns = session_->layout().columns();
    if (owner.hasType(ids::track))
    {
        const int t = set.trackIndexForId(LiveSet::idOf(owner));
        if (t < 0)
            return;
        selected.set({SessionSelection::Kind::track, t, juce::jmax(0, selected.get().scene)});
        for (int c = 0; c < static_cast<int>(columns.size()); ++c)
            if (columns[static_cast<std::size_t>(c)].kind == SessionLayout::Column::Kind::track && columns[static_cast<std::size_t>(c)].index == t)
                column = c;
    }
    else if (owner.hasType(ids::returnTrack))
    {
        const int r = set.returnIndexForId(LiveSet::idOf(owner));
        for (int c = 0; c < static_cast<int>(columns.size()); ++c)
            if (columns[static_cast<std::size_t>(c)].kind == SessionLayout::Column::Kind::returnTrack && columns[static_cast<std::size_t>(c)].index == r)
                column = c;
    }
    tabs.setCurrentTabIndex(0);
    if (!session_->isMixerVisible())
    {
        session_->setMixerVisible(true);
        settings.setValue(mixerKey, true);
    }
    const juce::StringArray ioProperties{"tuningId", "highNotePolicy", "bendMode", "bendRange", "midiInput", "midiChannel", "monitoring"};
    if (ioProperties.contains(property) && !session_->isIOVisible())
    {
        session_->setIOVisible(true);
        settings.setValue(ioKey, true);
    }
    if (property.startsWith("send:") && !session_->areSendsVisible())
    {
        session_->setSendsVisible(true);
        settings.setValue(sendsKey, true);
    }
    session_->flushUpdates();
    auto* strip = owner.hasType(ids::master) ? session_->masterStrip() : session_->stripForColumn(column);
    if (strip == nullptr)
        return;
    // The control in the strip: by what it's bound to.
    const auto wantedTarget = property.startsWith("send:") ? "send:" + juce::String(LiveSet::idOf(owner)) + "/" + property.fromFirstOccurrenceOf(":", false, false)
                                                           : ValueBinding::targetOf(owner, property);
    const juce::StringArray specIds = property == "tuningId"     ? juce::StringArray{"tuning"}
                                      : property == "bendRange"  ? juce::StringArray{"bendRangeCents", "bendRangeSteps"}
                                      : property == "monitoring" ? juce::StringArray{"monitoringAuto"}
                                                                 : juce::StringArray{property};
    juce::Component* found = nullptr;
    std::function<void(juce::Component&)> search = [&](juce::Component& c) {
        if (auto* bound = dynamic_cast<BoundControl*>(&c); bound != nullptr && found == nullptr && c.isVisible())
            if (bound->binding().target == wantedTarget || specIds.contains(bound->binding().spec().id))
                found = &c;
        if (auto* toggle = dynamic_cast<Toggle*>(&c); toggle != nullptr && found == nullptr && toggle->binding() != nullptr && c.isVisible())
            if (toggle->binding()->target == wantedTarget || specIds.contains(toggle->binding()->spec().id))
                found = &c;
        for (auto* child : c.getChildren())
            search(*child);
    };
    search(*strip);
    flash(found);
}

void MainShell::flash(juce::Component* control)
{
    flashed = control;
    repaint();
    juce::Component::SafePointer<MainShell> safe(this);
    juce::Timer::callAfterDelay(1500, [safe, control] {
        if (safe != nullptr && safe->flashed.getComponent() == control)
        {
            safe->flashed = nullptr;
            safe->repaint();
        }
    });
}

void MainShell::setMapMode(Mappings::Mode mode)
{
    mapper->setMode(mode);
    const bool on = mode != Mappings::Mode::off;
    overlay->setVisible(on);
    mappingList->setVisible(on);
    mappingList->refresh();
    if (on)
    {
        overlay->toFront(mode == Mappings::Mode::key); // Key Map: it has the keys
        if (mode == Mappings::Mode::key && overlay->isShowing())
            overlay->grabKeyboardFocus();
    }
    resized();
    repaint();
}

void MainShell::applyMidiInputs()
{
    engine.setRemoteInputs(prefs::remoteInputs(settings), prefs::nonTrackInputs(settings));
}

void MainShell::changeListenerCallback(juce::ChangeBroadcaster*)
{
    prefs::saveShortcuts(settings, registry);
}

PreferencesView::Context MainShell::preferencesContext()
{
    PreferencesView::Context c{.session = session,
                               .engine = engine,
                               .settings = settings,
                               .commands = registry,
                               .tunings = deviceShared->tunings,
                               .library = *libraryService,
                               .keyboard = computerKeyboard,
                               .audioError = audioError};
    c.setTheme = [this](const Theme& t) { setTheme(t); };
    c.controllersChanged = [this] { applyVirtualPushSettings(); };
    c.theme = [this]() -> const Theme& { return theme(); };
    c.keyboardChanged = [this] {
        if (computerKeyboard != nullptr)
            prefs::applyKeyboard(settings, *computerKeyboard);
        if (onKeyboardSettingsChanged)
            onKeyboardSettingsChanged();
    };
    c.chooseFolder = [this](std::function<void(const juce::File&)> chosen) {
        chooser = std::make_unique<juce::FileChooser>("Add a folder to Places", juce::File::getSpecialLocation(juce::File::userMusicDirectory));
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                             [chosen = std::move(chosen)](const juce::FileChooser& fc) {
                                 if (fc.getResult().isDirectory())
                                     chosen(fc.getResult());
                             });
    };
    c.chooseFile = [this](const juce::String& title, const juce::String& patterns, bool save, std::function<void(const juce::File&)> chosen) {
        chooser = std::make_unique<juce::FileChooser>(title, juce::File::getSpecialLocation(juce::File::userDocumentsDirectory), patterns);
        const int mode = save ? juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles |
                                     juce::FileBrowserComponent::warnAboutOverwriting
                               : juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles;
        chooser->launchAsync(mode, [chosen = std::move(chosen)](const juce::FileChooser& fc) {
            if (fc.getResult() != juce::File())
                chosen(fc.getResult());
        });
    };
    c.themesFolder = platform::userDataFolder().getChildFile("Themes");
    c.midiChanged = [this] { applyMidiInputs(); };
    return c;
}

void MainShell::showPreferences()
{
    showPreferences(static_cast<PreferencesView::Page>(
        juce::jlimit(0, PreferencesView::numPages - 1, settings.getIntValue(PreferencesView::lastPageKey, 0))));
}

void MainShell::showPreferences(PreferencesView::Page page)
{
    if (preferencesWindow == nullptr)
    {
        // A window of its own, beside the main one; closing it hides it.
        struct Window final : juce::DocumentWindow
        {
            explicit Window(const Theme& t) : juce::DocumentWindow("Preferences", t.surface, juce::DocumentWindow::closeButton) {}
            void closeButtonPressed() override { setVisible(false); }
        };
        preferencesWindow = std::make_unique<Window>(theme());
        preferencesWindow->setUsingNativeTitleBar(true);
        preferencesWindow->setLookAndFeel(&lookAndFeel);
        preferencesWindow->setContentOwned(new PreferencesView(preferencesContext()), false);
        preferencesWindow->setResizable(true, false);
        preferencesWindow->centreAroundComponent(this, juce::roundToInt(theme().controlHeight * 40.0f), juce::roundToInt(theme().controlHeight * 26.0f));
    }
    if (auto* view = preferences())
        view->show(page);
    preferencesWindow->setVisible(true);
    preferencesWindow->toFront(true);
}

PreferencesView* MainShell::preferences() noexcept
{
    return preferencesWindow != nullptr ? dynamic_cast<PreferencesView*>(preferencesWindow->getContentComponent()) : nullptr;
}

void MainShell::setComputerKeyboard(ComputerKeyboardInput* keyboard)
{
    computerKeyboard = keyboard;
    bar->setComputerKeyboard(keyboard);
    if (keyboard != nullptr)
        prefs::applyKeyboard(settings, *keyboard);
}

void MainShell::setUndoHistoryVisible(bool visible)
{
    showHistory = visible;
    settings.setValue(historyKey, visible);
    history->setVisible(visible);
    resized();
}

void MainShell::showMissingFiles(std::vector<setfiles::Reference> missing)
{
    if (missingWindow != nullptr)
        delete missingWindow.getComponent();
    auto* view = new MissingFilesView(session, libraryService->index(), std::move(missing));
    view->setLookAndFeel(&lookAndFeel);
    juce::Component::SafePointer<MissingFilesView> safeView(view);
    view->locate = [this, safeView](int entry) {
        if (safeView == nullptr)
            return;
        const auto wanted = safeView->entries()[static_cast<std::size_t>(entry)].file;
        chooser = std::make_unique<juce::FileChooser>("Where is " + wanted.getFileName() + "?", wanted.getParentDirectory(),
                                                      "*" + wanted.getFileExtension());
        chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                             [safeView, entry](const juce::FileChooser& fc) {
                                 if (safeView != nullptr && fc.getResult().existsAsFile())
                                     safeView->relink(entry, fc.getResult());
                             });
    };
    view->onClose = [this] {
        if (missingWindow != nullptr)
            missingWindow->exitModalState(0);
    };
    juce::DialogWindow::LaunchOptions options;
    options.dialogTitle = "Missing Files";
    options.content.setOwned(view);
    options.componentToCentreAround = this;
    options.dialogBackgroundColour = theme().surface;
    options.escapeKeyTriggersCloseButton = true;
    options.useNativeTitleBar = true;
    options.resizable = true;
    missingWindow = options.launchAsync();
}

void MainShell::addTuningCommands()
{
    using juce::KeyPress;
    using juce::ModifierKeys;
    const auto editing = [this] { return tunings_ != nullptr && tunings_->isEditing(); };
    const auto hasTrack = [this] {
        const auto s = selected.get();
        return s.kind != SessionSelection::Kind::scene && s.track >= 0 && s.track < session.set().numTracks() &&
               !session.set().isGroupTrack(s.track);
    };
    registry.add({.id = "view.tuning",
                  .name = "Show Tuning View",
                  .category = "View",
                  .description = "Shows the selected track's tuning in the detail view: every entry, and the tuning editor.",
                  .defaultKeys = {KeyPress('t', ModifierKeys::commandModifier | ModifierKeys::altModifier, 0)},
                  .perform = [this] { showTuningView(); },
                  .isTicked = [this] { return showDetail && detail != nullptr && detail->page() == DetailView::Page::tuning; }});
    registry.add({.id = "tuning.load",
                  .name = "Load Tuning File...",
                  .category = "Tuning",
                  .description = "Loads a Scala (.scl) or Colundi (.txt) tuning file and gives it to the selected track.",
                  .perform = [this] { chooseTuningFile(selected.get().track); },
                  .isEnabled = hasTrack});
    registry.add({.id = "tuning.duplicate",
                  .name = "Duplicate Tuning to Edit",
                  .category = "Tuning",
                  .description = "Makes a copy of the selected track's tuning in the Tuning view, to edit and save as your own.",
                  .perform =
                      [this] {
                          showTuningView();
                          tunings_->duplicate();
                      },
                  .isEnabled = [hasTrack, editing] { return hasTrack() && !editing(); }});
    registry.add({.id = "tuning.edit",
                  .name = "Edit Tuning",
                  .category = "Tuning",
                  .description = "Edits the selected track's tuning in the Tuning view, if it's one you saved from the editor.",
                  .perform =
                      [this] {
                          showTuningView();
                          tunings_->editInPlace();
                      },
                  .isEnabled = [this] { return tunings_ != nullptr && tunings_->canEditInPlace(); }});
    registry.add({.id = "tuning.save",
                  .name = "Save Tuning",
                  .category = "Tuning",
                  .description = "Saves the tuning being edited to your Tunings folder and gives it to its track.",
                  .perform = [this] { tunings_->save(); },
                  .isEnabled = editing});
    registry.add({.id = "tuning.discard",
                  .name = "Discard Tuning Changes",
                  .category = "Tuning",
                  .description = "Drops the tuning being edited; its track plays its own tuning again.",
                  .perform = [this] { tunings_->discard(); },
                  .isEnabled = editing});
    registry.add({.id = "tuning.copyTable",
                  .name = "Copy Tuning Table",
                  .category = "Tuning",
                  .description = "Copies the Tuning view's rows (or the selected ones) as tab-separated text.",
                  .perform = [this] { tunings_->copyToClipboard(); },
                  .isEnabled = [this] { return tunings_ != nullptr && tunings_->shown() != nullptr; }});
}

juce::File MainShell::userTuningsFolder() const
{
    return libraryService->userLibrary().getChildFile("Tunings");
}

void MainShell::showTuningView()
{
    if (!showDetail)
        setDetailVisible(true);
    detail->show(DetailView::Page::tuning);
    tunings_->flushUpdates();
    if (tunings_->tableComponent().isShowing())
        tunings_->tableComponent().grabKeyboardFocus();
}

void MainShell::chooseTuningFile(int track)
{
    if (track < 0 || track >= session.set().numTracks() || session.set().isGroupTrack(track))
        return;
    const auto trackId = session.set().trackId(track);
    chooser = std::make_unique<juce::FileChooser>("Load a tuning file", juce::File::getSpecialLocation(juce::File::userMusicDirectory),
                                                  "*.scl;*.txt");
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [this, trackId, before = focusToRestore()](const juce::FileChooser& fc) {
                             // By ID: the set may have changed while the dialog was open.
                             auto& set = session.set();
                             for (int t = 0; t < set.numTracks(); ++t)
                                 if (set.trackId(t) == trackId && fc.getResult().existsAsFile())
                                     setTrackTuningFrom(t, fc.getResult().getFullPathName());
                             restoreFocusLater(before);
                         });
}

void MainShell::askText(const juce::String& title, const juce::String& message, const juce::String& initial,
                        std::function<void(const juce::String&)> answer)
{
    auto* window = new juce::AlertWindow(title, message, juce::MessageBoxIconType::QuestionIcon, this);
    window->addTextEditor("answer", initial);
    window->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->enterModalState(true,
                            juce::ModalCallbackFunction::create([window, answer = std::move(answer), this, before = focusToRestore()](int result) {
                                if (result == 1 && answer)
                                    answer(window->getTextEditorContents("answer"));
                                restoreFocusLater(before);
                            }),
                            true);
}

void MainShell::setAutoArm(bool on)
{
    autoArm = on;
    settings.setValue(autoArmKey, on);
    selectionChanged(selected.get());
}

void MainShell::selectionChanged(const SessionSelection& now)
{
    auto& set = session.set();
    const bool midiTrack = now.kind != SessionSelection::Kind::scene && now.track >= 0 && now.track < set.numTracks() &&
                           !set.isGroupTrack(now.track);
    session.setAutoArmTrack(autoArm && midiTrack ? set.trackId(now.track) : 0);
}

void MainShell::audioDeviceOpened(const juce::String& error)
{
    audioError = error;
    if (error.isNotEmpty() && deviceShared->confirm)
        deviceShared->confirm("No audio device", error + "\n\nChoose one in Preferences, on the Audio page.", {}, nullptr);
}

void MainShell::setTheme(const Theme& t)
{
    lookAndFeel.setTheme(t);
    settings.setValue(themeKey, t.name);
    for (int i = 0; i < tabs.getNumTabs(); ++i)
        tabs.setTabBackgroundColour(i, t.background);
    sendLookAndFeelChange();
    if (virtualSurface != nullptr)
        virtualSurface->setColours(surfaceColours());
    if (virtualPushWindow != nullptr)
        virtualPushWindow->setBackgroundColour(t.surface);
    resized(); // sizes come from the theme too
    repaint();
}

void MainShell::setBrowserVisible(bool visible)
{
    showBrowser = visible;
    settings.setValue(browserKey, visible);
    browser->setVisible(visible);
    resized();
}

void MainShell::setDetailVisible(bool visible)
{
    showDetail = visible;
    settings.setValue(detailKey, visible);
    detail->setVisible(visible);
    resized();
}

void MainShell::showClipView()
{
    clip->flushUpdates();
    if (!showDetail)
        setDetailVisible(true);
    detail->show(DetailView::Page::clip);
    if (clip->pianoRoll().isShowing())
        clip->pianoRoll().grabKeyboardFocus();
}

bool MainShell::setTrackTuningFrom(int track, const juce::String& idOrPath)
{
    auto& set = session.set();
    if (track < 0 || track >= set.numTracks() || LiveSet::isGroup(set.track(track)))
        return false;
    auto& tunings = deviceShared->tunings;
    TuningPtr tuning = tunings.findById(idOrPath);
    if (tuning == nullptr && juce::File::isAbsolutePath(idOrPath))
    {
        // A tuning file (a few kilobytes) the registry hasn't seen: it's read once, here.
        const auto loaded = tunings.loadFile(juce::File(idOrPath));
        if (!loaded.ok())
        {
            if (deviceShared->confirm)
                deviceShared->confirm("Can't use that tuning", loaded.error, {}, nullptr);
            return false;
        }
        tuning = loaded.table;
        prefs::rememberTuningFile(settings, juce::File(idOrPath)); // and loaded again next time
    }
    if (tuning == nullptr)
        return false;
    session.beginEdit(undoNames::withValue("Tuning", tuning->name), set.track(track));
    set.setTrackTuning(track, tuning->id);
    return true;
}

bool MainShell::insertFromBrowser(const juce::String& description)
{
    if (drag::is(description, "tuning"))
        return setTrackTuningFrom(selected.get().track, drag::payload(description));
    devices->flushUpdates();
    const bool inserted = devices->insertHere(description);
    showDeviceView();
    return inserted;
}

bool MainShell::dropFromBrowser(const SessionView::Hit& hit, const juce::String& description)
{
    auto& set = session.set();
    DeviceOwner owner;
    if (hit.area == SessionView::Hit::Area::master)
        owner = {DeviceOwner::Kind::master, 0};
    else
    {
        const auto& columns = session_->layout().columns();
        if (hit.column < 0 || hit.column >= static_cast<int>(columns.size()))
            return false;
        const auto& c = columns[static_cast<std::size_t>(hit.column)];
        owner = {c.kind == SessionLayout::Column::Kind::returnTrack ? DeviceOwner::Kind::returnTrack : DeviceOwner::Kind::track, c.index};
    }
    if (drag::is(description, "tuning"))
        return owner.kind == DeviceOwner::Kind::track && setTrackTuningFrom(owner.index, drag::payload(description));

    // The track (or return, or the master) shows its devices, and the new one goes at the end.
    juce::ValueTree target;
    if (owner.kind == DeviceOwner::Kind::track)
    {
        selected.set({SessionSelection::Kind::track, owner.index, juce::jmax(0, selected.get().scene)});
        target = set.track(owner.index);
    }
    else
    {
        selected.showDevicesOf(owner);
        target = owner.kind == DeviceOwner::Kind::master ? set.master() : set.returnTrack(owner.index);
    }
    devices->flushUpdates();
    const bool inserted = devices->insert(description, LiveSet::chainOf(target), -1);
    showDeviceView();
    return inserted;
}

std::vector<MainShell::FocusArea> MainShell::focusAreas() const
{
    const auto inside = [](juce::Component* area) {
        return [area](const juce::Component* c) { return c != nullptr && (c == area || area->isParentOf(c)); };
    };
    const auto first = [](juce::Component* area) { return [area] { return accessible::focusFirstIn(*area); }; };
    std::vector<FocusArea> areas;
    areas.push_back({"Control bar", inside(bar.get()), first(bar.get())});
    if (showBrowser)
        areas.push_back({"Browser", inside(browser.get()), first(&browser->listComponent())});
    areas.push_back({"Session",
                     [this, in = inside(session_.get())](const juce::Component* c) { return in(c) && !session_->isInMixer(c); },
                     first(session_.get())});
    if (session_->isMixerVisible())
        areas.push_back({"Mixer", [this](const juce::Component* c) { return session_->isInMixer(c); },
                         [this] {
                             // The selected track's strip first: the selection is where you're working.
                             if (auto* strip = session_->stripForColumn(selected.get().track); strip != nullptr && accessible::focusFirstIn(*strip))
                                 return true;
                             for (int c = 0; auto* strip = session_->stripForColumn(c); ++c)
                                 if (accessible::focusFirstIn(*strip))
                                     return true;
                             return session_->masterStrip() != nullptr && accessible::focusFirstIn(*session_->masterStrip());
                         }});
    if (showDetail)
        areas.push_back({"Detail", inside(detail.get()), [this] {
                             switch (detail->page())
                             {
                             case DetailView::Page::clip: return accessible::focusFirstIn(clip->pianoRoll());
                             case DetailView::Page::device: return accessible::focusFirstIn(*devices);
                             case DetailView::Page::tuning: return accessible::focusFirstIn(tunings_->tableComponent());
                             }
                             return false;
                         }});
    if (showHistory)
        areas.push_back({"Undo history", inside(history.get()), first(history.get())});
    return areas;
}

void MainShell::moveFocusToArea(int direction)
{
    const auto areas = focusAreas();
    const int n = static_cast<int>(areas.size());
    const auto* focused = juce::Component::getCurrentlyFocusedComponent();
    int at = -1;
    for (int i = 0; i < n; ++i)
        if (areas[static_cast<std::size_t>(i)].contains(focused))
            at = i;
    // From nowhere, F6 starts at the first area and Shift+F6 at the last.
    int next = at < 0 ? (direction > 0 ? 0 : n - 1) : at + direction;
    for (int tries = 0; tries < n; ++tries, next += direction)
    {
        const auto& area = areas[static_cast<std::size_t>((next % n + n) % n)];
        if (area.focus())
        {
            juce::AccessibilityHandler::postAnnouncement(area.name, juce::AccessibilityHandler::AnnouncementPriority::high);
            return;
        }
    }
}

juce::String MainShell::focusedArea() const
{
    juce::String name;
    for (const auto& area : focusAreas())
        if (area.contains(juce::Component::getCurrentlyFocusedComponent()))
            name = area.name;
    return name;
}

juce::Component::SafePointer<juce::Component> MainShell::focusToRestore() const
{
    return juce::Component::getCurrentlyFocusedComponent();
}

void MainShell::restoreFocusLater(juce::Component::SafePointer<juce::Component> before)
{
    // An asynchronous dialog doesn't give the focus back when it closes (only a modal loop would),
    // and Windows may hand it to the window itself, above the shell's key mappings. Once the
    // dialog has gone, put it back where it was, or on the Device view.
    juce::Component::SafePointer<MainShell> safe(this);
    juce::MessageManager::callAsync([before, safe] {
        if (safe == nullptr)
            return;
        if (auto* now = juce::Component::getCurrentlyFocusedComponent(); now != nullptr && safe->isParentOf(now))
            return; // the answer moved it already (an inserted device selects itself)
        if (before != nullptr && before->isShowing() && safe->isParentOf(before))
            before->grabKeyboardFocus();
        else if (safe->devices != nullptr && safe->devices->isShowing())
            safe->devices->grabKeyboardFocus();
        else
            safe->grabKeyboardFocus();
    });
}

void MainShell::showDeviceView()
{
    if (!showDetail)
        setDetailVisible(true);
    detail->show(DetailView::Page::device);
}

void MainShell::mouseDown(const juce::MouseEvent& e)
{
    const auto at = e.getEventRelativeTo(this).getPosition();
    dragStartHeight = showDetail && detailDivider.contains(at) ? detail->getHeight() : -1;
    dragStartWidth = showBrowser && browserDivider.contains(at) ? browser->getWidth() : -1;
    dragStartPushWidth = pushDivider.contains(at) && virtualPush != nullptr ? virtualPush->getWidth() : -1;
}

void MainShell::mouseDrag(const juce::MouseEvent& e)
{
    if (dragStartPushWidth >= 0)
    {
        pushWidth = dragStartPushWidth - e.getDistanceFromDragStartX();
        resized();
        return;
    }
    if (dragStartWidth >= 0)
    {
        browserWidth = dragStartWidth + e.getDistanceFromDragStartX();
        resized();
        return;
    }
    if (dragStartHeight < 0)
        return;
    detailHeight = dragStartHeight - e.getDistanceFromDragStartY();
    resized();
}

void MainShell::mouseUp(const juce::MouseEvent&)
{
    if (dragStartHeight >= 0)
        settings.setValue(detailHeightKey, detail->getHeight());
    if (dragStartWidth >= 0)
        settings.setValue(browserWidthKey, browser->getWidth());
    if (dragStartPushWidth >= 0 && virtualPush != nullptr)
        settings.setValue(virtualPushWidthKey, virtualPush->getWidth());
    dragStartHeight = -1;
    dragStartWidth = -1;
    dragStartPushWidth = -1;
}

void MainShell::setInfoVisible(bool visible)
{
    showInfo = visible;
    settings.setValue(infoKey, visible);
    info.setVisible(visible);
    resized();
}

void MainShell::showInfoFor(const juce::Component* component)
{
    if (showInfo)
        info.show(infoFor(component, registry));
}

void MainShell::mouseMove(const juce::MouseEvent& e)
{
    if (e.eventComponent != nullptr && (e.eventComponent == this || isParentOf(e.eventComponent)))
        showInfoFor(e.eventComponent);
    if (e.eventComponent == this)
        setMouseCursor(showDetail && detailDivider.contains(e.getPosition())    ? juce::MouseCursor::UpDownResizeCursor
                       : (showBrowser && browserDivider.contains(e.getPosition())) || pushDivider.contains(e.getPosition())
                           ? juce::MouseCursor::LeftRightResizeCursor
                                                                                 : juce::MouseCursor::NormalCursor);
}

// --- Virtual Push ----------------------------------------------------------------------------------------

/** Virtual Push in its own window: closing it hides Virtual Push. */
class MainShell::VirtualPushWindow final : public juce::DocumentWindow
{
public:
    VirtualPushWindow(MainShell& s, juce::Colour background)
        : juce::DocumentWindow("Virtual Push", background, juce::DocumentWindow::closeButton | juce::DocumentWindow::minimiseButton),
          shell(s)
    {
        setUsingNativeTitleBar(true);
        setResizable(true, false);
    }
    void closeButtonPressed() override
    {
        // Not from inside this window's own callback: hiding Virtual Push deletes the window.
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<MainShell>(&shell)] {
            if (safe != nullptr)
                safe->setVirtualPushVisible(false);
        });
    }

private:
    MainShell& shell;
};

void MainShell::addControllerCommands()
{
    using juce::KeyPress;
    using juce::ModifierKeys;
    registry.add({.id = "view.virtualPush",
                  .name = "Show Virtual Push",
                  .category = "View",
                  .description = "Shows or hides Virtual Push: an on-screen controller whose pads launch clips, with the transport and the modes beside them.",
                  .defaultKeys = {KeyPress('p', ModifierKeys::commandModifier | ModifierKeys::altModifier, 0)},
                  .perform = [this] { setVirtualPushVisible(virtualPush == nullptr); },
                  .isTicked = [this] { return virtualPush != nullptr; }});
    registry.add({.id = "view.virtualPushWindow",
                  .name = "Virtual Push in Its Own Window",
                  .category = "View",
                  .description = "Shows Virtual Push in a window of its own (for a second screen or a touchscreen) instead of at the right of this one.",
                  .perform =
                      [this] {
                          setVirtualPushDocked(!pushDocked);
                          setVirtualPushVisible(true);
                      },
                  .isTicked = [this] { return !pushDocked; }});
}

control::SurfaceColours MainShell::surfaceColours() const
{
    const auto& t = theme();
    control::SurfaceColours c;
    c.play = control::rgbOf(t.play.getARGB());
    c.record = control::rgbOf(t.record.getARGB());
    c.arm = control::rgbOf(t.arm.getARGB());
    c.mute = control::rgbOf(t.mute.getARGB());
    c.solo = control::rgbOf(t.solo.getARGB());
    return c; // white stays white: it's the light's colour, not the text's
}

void MainShell::setVirtualPushVisible(bool visible)
{
    settings.setValue(virtualPushKey, visible);
    if (!visible)
    {
        if (virtualPushWindow != nullptr)
            settings.setValue(virtualPushBoundsKey, virtualPushWindow->getWindowStateAsString());
        virtualSurface.reset();
        virtualPushWindow.reset();
        virtualPush.reset();
        resized();
        return;
    }
    if (virtualPush != nullptr)
    {
        if (virtualPushWindow != nullptr)
            virtualPushWindow->toFront(false);
        return;
    }
    virtualPush = std::make_unique<control::VirtualPush>(stateFeed, virtualPushSettings());
    virtualSurface = std::make_unique<control::SurfaceController>(control::SurfaceContext{session, registry, selected, stateFeed, &deviceSpecs}, *virtualPush);
    virtualSurface->setColours(surfaceColours());
    virtualSurface->setExpression(expressionSettings());
    placeVirtualPush();
}

void MainShell::setVirtualPushDocked(bool docked)
{
    settings.setValue(virtualPushWindowKey, !docked);
    if (docked == pushDocked)
        return;
    pushDocked = docked;
    if (virtualPush != nullptr)
        placeVirtualPush();
}

void MainShell::placeVirtualPush()
{
    if (pushDocked)
    {
        if (virtualPushWindow != nullptr)
        {
            settings.setValue(virtualPushBoundsKey, virtualPushWindow->getWindowStateAsString());
            virtualPushWindow->clearContentComponent();
            virtualPushWindow.reset();
        }
        addAndMakeVisible(*virtualPush);
        resized();
        return;
    }
    removeChildComponent(virtualPush.get());
    virtualPushWindow = std::make_unique<VirtualPushWindow>(*this, theme().surface);
    virtualPushWindow->setLookAndFeel(&lookAndFeel);
    const auto& t = theme();
    // As tall as its layout is for the width (a display above the grid makes it taller).
    const float width = t.controlHeight * 30.0f;
    const auto shape = control::VirtualPush::layoutFor(virtualPush->capabilities(), {0.0f, 0.0f, width - t.padding * 2.0f, 1.0e6f});
    virtualPush->setSize(juce::roundToInt(width), juce::roundToInt(shape.used.getHeight() + t.padding * 2.0f));
    virtualPushWindow->setContentNonOwned(virtualPush.get(), true);
    const auto saved = settings.getValue(virtualPushBoundsKey);
    if (saved.isEmpty() || !virtualPushWindow->restoreWindowStateFromString(saved))
        virtualPushWindow->centreWithSize(virtualPushWindow->getWidth(), virtualPushWindow->getHeight());
    virtualPushWindow->setVisible(true);
    resized();
}

control::Expression MainShell::expressionSettings() const
{
    control::Expression e;
    e.velocityCurve = static_cast<float>(juce::jlimit(-100, 100, settings.getIntValue(prefs::velocityCurveKey, 0))) / 100.0f;
    e.pressure = static_cast<control::Expression::Pressure>(juce::jlimit(0, 2, settings.getIntValue(prefs::pressureKey, 1)));
    e.pressureThreshold = static_cast<float>(juce::jlimit(0, 50, settings.getIntValue(prefs::pressureThresholdKey, 0))) / 100.0f;
    e.strip = static_cast<control::Expression::Strip>(juce::jlimit(0, 2, settings.getIntValue(prefs::stripKey, 0)));
    return e;
}

control::VirtualPush::Settings MainShell::virtualPushSettings() const
{
    control::VirtualPush::Settings s;
    s.rows = juce::jlimit(4, 16, settings.getIntValue(prefs::virtualRowsKey, 8));
    s.columns = juce::jlimit(4, 16, settings.getIntValue(prefs::virtualColumnsKey, 8));
    s.fixedVelocity = juce::jlimit(0, 127, settings.getIntValue(prefs::virtualVelocityKey, 0));
    s.encoders = control::VirtualPush::encoderCountFor(settings.getIntValue(prefs::virtualEncodersKey, 8));
    return s;
}

void MainShell::applyVirtualPushSettings()
{
    if (virtualPush == nullptr)
        return;
    if (virtualPush->settings() == virtualPushSettings())
    {
        virtualSurface->setExpression(expressionSettings()); // the same device: only how it plays
        return;
    }
    setVirtualPushVisible(false);
    setVirtualPushVisible(true);
}

void MainShell::tickMapper()
{
    constexpr int idleFramesBeforeStopping = 30; // half a second at 60 Hz
    if (mapper->tick())
        mapperIdleFrames = 0;
    else if (++mapperIdleFrames == idleFramesBeforeStopping)
        // Not from inside the attachment's own callback.
        juce::MessageManager::callAsync([safe = juce::Component::SafePointer<MainShell>(this)] {
            if (safe != nullptr && safe->mapperIdleFrames >= idleFramesBeforeStopping)
                safe->mapperTick.reset();
        });
}

void MainShell::handleAsyncUpdate()
{
    status->refresh();
    if (mappingList != nullptr && mappingList->isVisible())
    {
        mappingList->refresh();
        overlay->repaint();
    }
}

void MainShell::resetLoadStatistics()
{
    engine.callbackStats().reset();
    frames.reset();
}

void MainShell::paint(juce::Graphics& g)
{
    // Everything in the window paints between here and paintOverChildren().
    paintStarted = juce::Time::getHighResolutionTicks();
    g.fillAll(lookAndFeel.theme().background);
}

void MainShell::paintOverChildren(juce::Graphics& g)
{
    // Find's flash: a ring round the control it found.
    if (auto* c = flashed.getComponent(); c != nullptr && c->isShowing() && isParentOf(c))
    {
        const auto& t = lookAndFeel.theme();
        g.setColour(t.focus);
        g.drawRoundedRectangle(getLocalArea(c, c->getLocalBounds()).toFloat().expanded(2.0f), t.radius, juce::jmax(2.0f, t.stroke * 2.0f));
    }
    const double painting = juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - paintStarted);
    frames.recordCallback(painting + stateFeed.takeTickWorkSeconds(), frameBudgetSeconds);
}

void MainShell::resized()
{
    const auto& t = lookAndFeel.theme();
    auto area = getLocalBounds();
    bar->setBounds(area.removeFromTop(bar->preferredHeight()));
    status->setBounds(area.removeFromBottom(status->preferredHeight()));

    const int gap = juce::roundToInt(t.spacing);
    // The browser's width is dragged at its right edge; the main area keeps most of the window.
    const int minimumSide = juce::roundToInt(t.controlHeight * 10.0f);
    const int defaultSide = juce::roundToInt(t.controlHeight * 17.0f);
    const int sideWidth = juce::jlimit(minimumSide, juce::jmax(minimumSide, area.getWidth() / 2),
                                       showBrowser ? (browserWidth > 0 ? browserWidth : defaultSide) : minimumSide);
    browserDivider = {};
    if (showBrowser || showInfo || showHistory)
    {
        auto side = area.removeFromLeft(sideWidth);
        if (showBrowser)
            browserDivider = area.removeFromLeft(gap);
        else
            area.removeFromLeft(gap);
        if (showInfo)
            info.setBounds(side.removeFromBottom(juce::roundToInt(t.fontNormal * 7.0f)));
        if (showHistory)
        {
            // Under the browser, a third of the column (all of it if the browser is hidden).
            history->setBounds(showBrowser ? side.removeFromBottom(side.getHeight() / 3) : side);
            if (showBrowser)
                side.removeFromBottom(gap);
        }
        if (showBrowser)
            browser->setBounds(side);
    }
    pushDivider = {};
    if (virtualPush != nullptr && pushDocked)
    {
        // Up to what leaves the main view room for a few tracks: the pads size to the width.
        const int minimum = juce::roundToInt(t.controlHeight * 12.0f);
        const int mainMinimum = juce::roundToInt(t.controlHeight * 16.0f);
        const int wanted = pushWidth > 0 ? pushWidth : area.getWidth() * 7 / 20;
        const int widest = juce::jmax(area.getWidth() / 2, area.getWidth() - mainMinimum);
        virtualPush->setBounds(area.removeFromRight(juce::jlimit(juce::jmin(minimum, area.getWidth() / 2), juce::jmax(minimum, widest), wanted)));
        pushDivider = area.removeFromRight(gap);
    }
    detailDivider = {};
    if (showDetail)
    {
        // The Clip view needs room for its controls; the Session view keeps at least as much.
        const int minimum = juce::roundToInt(t.controlHeight * 8.0f);
        const int wanted = detailHeight > 0 ? detailHeight : area.getHeight() * 2 / 5;
        const int height = juce::jlimit(juce::jmin(minimum, area.getHeight() / 2), juce::jmax(minimum, area.getHeight() - minimum), wanted);
        detail->setBounds(area.removeFromBottom(height));
        detailDivider = area.removeFromBottom(gap);
    }
    tabs.setBounds(area);
    if (mappingList != nullptr && mappingList->isVisible())
    {
        // While mapping, the list takes the browser's place.
        const auto side = browser->isVisible() ? browser->getBounds() : juce::Rectangle<int>(0, area.getY(), juce::roundToInt(t.controlHeight * 12.0f), area.getHeight());
        mappingList->setBounds(side);
        mappingList->toFront(false);
    }
    if (overlay != nullptr && overlay->isVisible())
        overlay->setBounds(tabs.getBounds().getUnion(detail->isVisible() ? detail->getBounds() : tabs.getBounds()));
}

} // namespace lattice::ui
