#include "ui/shell/MainShell.h"

#include "platform/Platform.h"
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

MainShell::MainShell(SessionController& s, AudioEngine& e, juce::PropertiesFile& settingsFile, TuningRegistry& tunings,
                     ShellActions actions)
    : session(s), engine(e), settings(settingsFile), registry(&s.undoManager()), stateFeed(s, e), deviceSpecs(s.deviceRegistry())
{
    setLookAndFeel(&lookAndFeel);
    if (const auto* saved = Theme::builtIn(settings.getValue(themeKey, "Dark")))
        lookAndFeel.setTheme(*saved);

    if (!actions.rename)
        actions.rename = [this] {
            if (session_ != nullptr)
                session_->renameSelection();
        };
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
    addKeyListener(&registry.keyMappings());
    menuModel = std::make_unique<CommandMenus>(registry);

    bar = std::make_unique<ControlBar>(session, registry, stateFeed, engine.workerThreads());
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
    status = std::make_unique<StatusBar>(session, jobPool);
    addAndMakeVisible(*status);
    jobPool.onJobsChanged = [this] { status->refresh(); };
    session.set().state().addListener(this);

    showBrowser = settings.getBoolValue(browserKey, false);
    showDetail = settings.getBoolValue(detailKey, false);
    detailHeight = settings.getIntValue(detailHeightKey, 0);
    browserWidth = settings.getIntValue(browserWidthKey, 0);
    showInfo = settings.getBoolValue(infoKey, true);
    browser->setVisible(showBrowser);
    detail->setVisible(showDetail);
    info.setVisible(showInfo);

    autoArm = settings.getBoolValue(autoArmKey, true);
    selected.addListener(this);
    selectionChanged(selected.get());

    stateFeed.attachTo(*this);
    juce::Desktop::getInstance().addGlobalMouseListener(this);
}

MainShell::~MainShell()
{
    session.onSetReplaced = nullptr;
    browser.reset(); // it plays through the engine and reads the library
    selected.removeListener(this);
    juce::Desktop::getInstance().removeGlobalMouseListener(this);
    session.set().state().removeListener(this);
    stateFeed.detach();
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

void MainShell::setDebugUi(juce::Component* debugUi)
{
    if (debugUi == nullptr)
        return;
    tabs.addTab("Debug UI", lookAndFeel.theme().background, debugUi, false);
}

void MainShell::setTheme(const Theme& t)
{
    lookAndFeel.setTheme(t);
    settings.setValue(themeKey, t.name);
    for (int i = 0; i < tabs.getNumTabs(); ++i)
        tabs.setTabBackgroundColour(i, t.background);
    sendLookAndFeelChange();
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
    }
    if (tuning == nullptr)
        return false;
    session.undoManager().beginNewTransaction("Tuning");
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
}

void MainShell::mouseDrag(const juce::MouseEvent& e)
{
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
    dragStartHeight = -1;
    dragStartWidth = -1;
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
                       : showBrowser && browserDivider.contains(e.getPosition()) ? juce::MouseCursor::LeftRightResizeCursor
                                                                                 : juce::MouseCursor::NormalCursor);
}

void MainShell::handleAsyncUpdate()
{
    status->refresh();
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

void MainShell::paintOverChildren(juce::Graphics&)
{
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
    if (showBrowser || showInfo)
    {
        auto side = area.removeFromLeft(sideWidth);
        if (showBrowser)
            browserDivider = area.removeFromLeft(gap);
        else
            area.removeFromLeft(gap);
        if (showInfo)
            info.setBounds(side.removeFromBottom(juce::roundToInt(t.fontNormal * 7.0f)));
        if (showBrowser)
            browser->setBounds(side);
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
}

} // namespace lattice::ui
