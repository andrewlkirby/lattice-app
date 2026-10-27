#pragma once

#include "core/JobPool.h"
#include "ui/commands/SessionCommands.h"
#include "ui/session/SessionSelectionModel.h"
#include "ui/session/SessionView.h"
#include "ui/feed/EngineStateFeed.h"
#include "ui/shell/CommandMenus.h"
#include "ui/shell/ControlBar.h"
#include "ui/shell/DetailView.h"
#include "ui/shell/InfoView.h"
#include "ui/shell/StatusBar.h"
#include "ui/theme/LatticeLookAndFeel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace lattice::ui
{

/** The main window's layout (docs/05-m4-ui.md, 4a): the control bar along the top, the browser on
    the left with the info view under it, the main area (Session; Arrangement, disabled for now;
    and, until the new views cover everything, the Debug UI), the detail view along the bottom, and
    the status bar. The browser, detail view and info view can each be shown or hidden.

    It owns what the whole UI shares: the look and feel and its theme, the command registry, the
    engine state feed (ticking on this window's display refresh) and the background job pool. Keys
    reach the commands first, through the registry's key mappings; whatever no command takes goes
    on to the window (the computer keyboard). */
class MainShell final : public juce::Component,
                        public ControlRegistry::Provider,
                        private SessionSelectionModel::Listener,
                        private juce::ValueTree::Listener,
                        private juce::AsyncUpdater
{
public:
    /** actions: the dialogs the window provides (a missing rename is the Session view's prompt). */
    MainShell(SessionController& session, AudioEngine& engine, juce::PropertiesFile& settings, TuningRegistry& tunings,
              ShellActions actions);
    ~MainShell() override;

    CommandRegistry& commands() noexcept { return registry; }
    /** Every control in the window, for Find, MIDI Map and Key Map. */
    ControlRegistry& controlRegistry() override { return controls; }
    /** The menu bar's model (the window shows it): every command, by category. */
    CommandMenus& menus() noexcept { return *menuModel; }
    EngineStateFeed& feed() noexcept { return stateFeed; }
    JobPool& jobs() noexcept { return jobPool; }
    ControlBar& controlBar() noexcept { return *bar; }
    InfoView& infoView() noexcept { return info; }
    StatusBar& statusBar() noexcept { return *status; }
    juce::TabbedComponent& mainArea() noexcept { return tabs; }
    /** What's selected in the Session view; the commands act on it. */
    SessionSelectionModel& selection() noexcept { return selected; }
    SessionView& sessionView() noexcept { return *session_; }
    DetailView& detailView() noexcept { return *detail; }
    ClipView& clipView() noexcept { return *clip; }
    /** Shows the Clip view with the selected clip, and gives it the keys (double-clicking a clip). */
    void showClipView();

    /** The old views, in a "Debug UI" tab, until the new ones cover everything they do. */
    void setDebugUi(juce::Component* debugUi);

    void setTheme(const Theme& theme);
    const Theme& theme() const noexcept { return lookAndFeel.theme(); }

    bool isBrowserVisible() const noexcept { return showBrowser; }
    bool isDetailVisible() const noexcept { return showDetail; }
    bool isInfoVisible() const noexcept { return showInfo; }
    void setBrowserVisible(bool visible);
    void setDetailVisible(bool visible);
    void setInfoVisible(bool visible);

    /** What the info view shows when the pointer is over `component` (the shell calls this on every
        mouse move inside its window; tests call it directly). */
    void showInfoFor(const juce::Component* component);

    /** The UI thread's work per frame (the feed's ticks and the painting), as shares of the frame
        budget (docs/09-performance-charter.md §1: a frame takes 8 ms or less while busy). */
    static constexpr double frameBudgetSeconds = 0.008;
    const AudioCallbackStats& frameStats() const noexcept { return frames; }
    void resetLoadStatistics();

    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;
    void resized() override;

    static constexpr auto themeKey = "theme";
    static constexpr auto browserKey = "view.browser";
    static constexpr auto detailKey = "view.detail";
    static constexpr auto detailHeightKey = "view.detailHeight";
    static constexpr auto infoKey = "view.info";
    static constexpr auto mixerKey = "view.mixer";
    static constexpr auto sendsKey = "view.sends";
    static constexpr auto ioKey = "view.io";
    static constexpr auto autoArmKey = "session.autoArm";

    /** Auto-arm (on by default): selecting a MIDI track arms it while no track is armed by hand,
        so the keyboards play what's selected (the M3 hands-on checks found the keyboard playing a
        track armed earlier). */
    bool isAutoArmOn() const noexcept { return autoArm; }
    void setAutoArm(bool on);

private:
    /** A panel that a later phase fills, saying which. */
    class Placeholder;

    void addViewCommands();
    void selectionChanged(const SessionSelection& now) override;
    /** Also the global mouse listener's: every move anywhere, shown if it's inside this window. */
    void mouseMove(const juce::MouseEvent&) override;
    /** Dragging the gap above the detail view resizes it. */
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override { triggerAsyncUpdate(); }
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { triggerAsyncUpdate(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override;

    SessionController& session;
    AudioEngine& engine;
    juce::PropertiesFile& settings;
    LatticeLookAndFeel lookAndFeel;
    CommandRegistry registry;
    SessionSelectionModel selected;
    ControlRegistry controls; // before the views, so it outlives their widgets
    EngineStateFeed stateFeed;
    JobPool jobPool;

    std::unique_ptr<CommandMenus> menuModel;
    std::unique_ptr<ControlBar> bar;
    std::unique_ptr<Placeholder> browser, devicePlaceholder, arrangementPlaceholder;
    std::unique_ptr<SessionView> session_;
    std::unique_ptr<ClipView> clip;
    std::unique_ptr<DetailView> detail; // after what it shows, so it goes first
    juce::Rectangle<int> detailDivider;
    int detailHeight = 0; // 0: the default share of the window
    int dragStartHeight = -1;
    juce::TabbedComponent tabs{juce::TabbedButtonBar::TabsAtTop};
    InfoView info;
    std::unique_ptr<StatusBar> status;
    bool showBrowser = false, showDetail = false, showInfo = true;
    bool autoArm = true;
    AudioCallbackStats frames;
    juce::int64 paintStarted = 0;
};

} // namespace lattice::ui
