#include "ui/shell/MainShell.h"

#include "ui/clip/ClipCommands.h"

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
    : session(s), engine(e), settings(settingsFile), registry(&s.undoManager()), stateFeed(s, e)
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
                        [this]() -> EditTarget* { return clip != nullptr ? clip->editTargetIfFocused() : nullptr; }},
                       std::move(actions));
    addViewCommands();
    addKeyListener(&registry.keyMappings());
    menuModel = std::make_unique<CommandMenus>(registry);

    bar = std::make_unique<ControlBar>(session, registry, stateFeed, engine.workerThreads());
    bar->frameSummary = [this] { return frames.summary(); };
    bar->frameBudgetSeconds = frameBudgetSeconds;
    addAndMakeVisible(*bar);

    browser = std::make_unique<Placeholder>("Browser", "Devices, presets, samples and tunings, with search (4e). For now the "
                                                       "device list is in the Debug UI tab.");
    devicePlaceholder = std::make_unique<Placeholder>("Device view", "Device panels come with 4d. For now the device editor is in "
                                                                     "the Debug UI tab. Shift+Tab switches back to the Clip view.");
    clip = std::make_unique<ClipView>(session, stateFeed, tunings, selected);
    detail = std::make_unique<DetailView>(*clip, *devicePlaceholder);
    addClipCommands(registry, clip->pianoRoll());
    clip->pianoRoll().setCommands(&registry);
    clip->pianoRoll().onHover = [this](const juce::String&) { showInfoFor(&clip->pianoRoll()); };
    session_ = std::make_unique<SessionView>(session, stateFeed, registry, selected, tunings);
    session_->setMixerVisible(settings.getBoolValue(mixerKey, true));
    session_->setSendsVisible(settings.getBoolValue(sendsKey, true));
    session_->setIOVisible(settings.getBoolValue(ioKey, true));
    session_->onOpenClip = [this] { showClipView(); };
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

void MainShell::mouseDown(const juce::MouseEvent& e)
{
    dragStartHeight = showDetail && detailDivider.contains(e.getEventRelativeTo(this).getPosition()) ? detail->getHeight() : -1;
}

void MainShell::mouseDrag(const juce::MouseEvent& e)
{
    if (dragStartHeight < 0)
        return;
    detailHeight = dragStartHeight - e.getDistanceFromDragStartY();
    resized();
}

void MainShell::mouseUp(const juce::MouseEvent&)
{
    if (dragStartHeight >= 0)
        settings.setValue(detailHeightKey, detail->getHeight());
    dragStartHeight = -1;
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
        setMouseCursor(showDetail && detailDivider.contains(e.getPosition()) ? juce::MouseCursor::UpDownResizeCursor
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
    const int sideWidth = juce::roundToInt(t.controlHeight * 10.0f);
    if (showBrowser || showInfo)
    {
        auto side = area.removeFromLeft(sideWidth);
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
