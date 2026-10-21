#include "ui/shell/MainShell.h"

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

MainShell::MainShell(SessionController& s, AudioEngine& e, juce::PropertiesFile& settingsFile, ShellActions actions)
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
    addSessionCommands(registry, session, {[this] { return selected.get(); }, [this](SessionSelection sel) { selected.set(sel); }},
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
    detail = std::make_unique<Placeholder>("Detail view", "The Clip view with the piano roll (4c) and the Device view with "
                                                          "device panels (4d). Shift+Tab will switch between them.");
    session_ = std::make_unique<SessionView>(session, stateFeed, registry, selected);
    session_->setMixerVisible(settings.getBoolValue(mixerKey, true));
    session_->setSendsVisible(settings.getBoolValue(sendsKey, true));
    session_->setIOVisible(settings.getBoolValue(ioKey, true));
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
    showInfo = settings.getBoolValue(infoKey, true);
    browser->setVisible(showBrowser);
    detail->setVisible(showDetail);
    info.setVisible(showInfo);

    stateFeed.attachTo(*this);
    juce::Desktop::getInstance().addGlobalMouseListener(this);
}

MainShell::~MainShell()
{
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
    registry.add({.id = "view.info",
                  .name = "Show Info View",
                  .category = "View",
                  .description = "Shows or hides the info view, which describes what's under the pointer.",
                  .defaultKeys = {KeyPress('/', ModifierKeys::shiftModifier, '?')}, // Shift+?, as in Live
                  .perform = [this] { setInfoVisible(!showInfo); },
                  .isTicked = [this] { return showInfo; }});
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
    if (showDetail)
    {
        detail->setBounds(area.removeFromBottom(juce::jmax(120, area.getHeight() * 3 / 10)));
        area.removeFromBottom(gap);
    }
    tabs.setBounds(area);
}

} // namespace lattice::ui
