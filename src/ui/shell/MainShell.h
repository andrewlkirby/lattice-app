#pragma once

#include "core/JobPool.h"
#include "library/Library.h"
#include "ui/browser/BrowserView.h"
#include "ui/device/DeviceView.h"
#include "ui/commands/SessionCommands.h"
#include "ui/session/SessionSelectionModel.h"
#include "ui/session/SessionView.h"
#include "ui/feed/EngineStateFeed.h"
#include "ui/shell/CommandMenus.h"
#include "ui/shell/ControlBar.h"
#include "ui/shell/DetailView.h"
#include "ui/shell/InfoView.h"
#include "ui/shell/MissingFilesView.h"
#include "ui/shell/SetFileActions.h"
#include "ui/shell/StatusBar.h"
#include "ui/shell/UndoHistoryView.h"
#include "ui/prefs/PreferencesView.h"
#include "ui/mapping/MapOverlay.h"
#include "ui/find/FindView.h"
#include "ui/theme/LatticeLookAndFeel.h"
#include "ui/tuning/TuningView.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace lattice::ui
{

/** The main window's layout (docs/05-m4-ui.md, 4a): the control bar along the top, the browser on
    the left (4e; its width dragged at its right edge) with the info view under it, the main area (Session; Arrangement, disabled for now),
    the detail view along the bottom, and the status bar. The browser, detail view and info view can each be shown or hidden.

    It owns what the whole UI shares: the look and feel and its theme, the command registry, the
    engine state feed (ticking on this window's display refresh) and the background job pool. Keys
    reach the commands first, through the registry's key mappings; whatever no command takes goes
    on to the window (the computer keyboard). */
class MainShell final : public juce::Component,
                        public ControlRegistry::Provider,
                        private juce::ChangeListener,
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
    DeviceView& deviceView() noexcept { return *devices; }
    TuningView& tuningView() noexcept { return *tunings_; }
    /** New, Open, Save, recent sets, autosave and the window's title. */
    SetFileActions& files() noexcept { return *setFiles; }
    UndoHistoryView& undoHistory() noexcept { return *history; }
    bool isUndoHistoryVisible() const noexcept { return showHistory; }
    void setUndoHistoryVisible(bool visible);
    /** The missing-files window for a set just opened (the default for files().onMissingFiles). */
    void showMissingFiles(std::vector<setfiles::Reference> missing);

    /** F6 and Shift+F6 (docs/05-m4-ui.md, 4h): the keys go to the next or previous area of the
        window, of those shown: the control bar, the browser, the Session grid, the mixer, the
        detail view (the piano roll, the Device view or the tuning table), the undo history. */
    void moveFocusToArea(int direction);
    /** The area the keys are in now (as F6 names them: "Session", "Mixer"...), or empty. */
    juce::String focusedArea() const;

    /** MIDI Map and Key Map (Ctrl+M, Ctrl+K): the mappings, and the mode. */
    Mappings& mappings() noexcept { return *mapper; }
    void setMapMode(Mappings::Mode mode);
    MapOverlay& mapOverlay() noexcept { return *overlay; }
    MappingsView& mappingsView() noexcept { return *mappingList; }
    /** The inputs MIDI Map listens to, and those open only for it, from the settings to the engine. */
    void applyMidiInputs();

    /** Find (Ctrl+F) and the command palette (Ctrl+Shift+P). */
    FindIndex& findIndex() noexcept { return *finder; }
    FindView& findView() noexcept { return *findBox; }
    void openFind(bool commandsOnly);
    /** Takes you to a result: reveals and flashes the control, opens a setting's tab, runs a command. */
    void goTo(const FindIndex::Result& result);
    /** The control flashed last (Find's), while it's flashing. */
    juce::Component* flashedControl() const noexcept { return flashed.getComponent(); }

    /** The Preferences window (Ctrl+,), on a tab. */
    void showPreferences(PreferencesView::Page page);
    void showPreferences();
    PreferencesView* preferences() noexcept;
    /** What the Preferences work on (tests make the view with it, without a window). */
    PreferencesView::Context preferencesContext();
    /** The computer keyboard (the app's): the Preferences' note keys and range steps apply to it. */
    void setComputerKeyboard(ComputerKeyboardInput* keyboard);
    /** After the note keys or a range step changed (the app re-reads the track's tuning). */
    std::function<void()> onKeyboardSettingsChanged;
    BrowserView& browserView() noexcept { return *browser; }
    library::Library& library() noexcept { return *libraryService; }
    /** Inserts what the browser names (a drag description) where the Device view is looking; a
        tuning goes to the selected track. False if nothing was inserted (or a question was asked). */
    bool insertFromBrowser(const juce::String& description);
    /** Gives a track a tuning: a TuningRegistry ID, or a tuning file (loaded first). One undo step. */
    bool setTrackTuningFrom(int track, const juce::String& idOrPath);
    /** Something from the browser dropped on the Session view: onto a track (or a return, or the
        master corner) it goes at the end of that chain; a tuning sets a track's tuning. */
    bool dropFromBrowser(const SessionView::Hit& hit, const juce::String& description);
    DeviceContext& deviceContext() noexcept { return *deviceShared; }
    /** Shows the Device view (with the selected track's devices). */
    void showDeviceView();
    /** Shows the Clip view with the selected clip, and gives it the keys (double-clicking a clip). */
    void showClipView();
    /** Shows the Tuning view (with the selected track's tuning). */
    void showTuningView();
    /** Asks for a tuning file (.scl or Colundi .txt) and gives it to a track. */
    void chooseTuningFile(int track);
    /** Where the tuning editor saves (the User Library's Tunings folder, which the browser shows). */
    juce::File userTuningsFolder() const;

    /** The audio device has been opened, or failed to open (error says why; the Preferences'
        Audio page shows it too). */
    void audioDeviceOpened(const juce::String& error);

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
    static constexpr auto browserWidthKey = "view.browserWidth";
    static constexpr auto infoKey = "view.info";
    static constexpr auto historyKey = "view.undoHistory";
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
    /** The focus a dialog about to open should give back (restoreFocusLater). */
    juce::Component::SafePointer<juce::Component> focusToRestore() const;
    void restoreFocusLater(juce::Component::SafePointer<juce::Component> before);

    /** A panel that a later phase fills, saying which. */
    class Placeholder;
    class ShiftTabBack;

    struct FocusArea
    {
        juce::String name;
        std::function<bool(const juce::Component*)> contains;
        std::function<bool()> focus;
    };
    std::vector<FocusArea> focusAreas() const;

    void addViewCommands();
    void addTuningCommands();
    void addFileCommands();
    void addMappingCommands();
    void addFindCommands();
    void revealMixerControl(const juce::ValueTree& owner, const juce::String& property);
    void flash(juce::Component* control);
    void changeListenerCallback(juce::ChangeBroadcaster*) override; // the shortcuts changed: saved
    /** A question with a text answer (an asynchronous dialog). */
    void askText(const juce::String& title, const juce::String& message, const juce::String& initial,
                 std::function<void(const juce::String&)> answer);
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
    std::unique_ptr<Placeholder> arrangementPlaceholder;
    std::unique_ptr<SessionView> session_;
    std::unique_ptr<ClipView> clip;
    DeviceSpecs deviceSpecs;
    std::unique_ptr<presets::PresetLibrary> presetLibrary;
    std::unique_ptr<library::Library> libraryService; // after the presets and the job pool it uses
    std::unique_ptr<BrowserView> browser;
    std::unique_ptr<DeviceContext> deviceShared;
    std::unique_ptr<DeviceView> devices;
    std::unique_ptr<TuningView> tunings_;
    std::unique_ptr<SetFileActions> setFiles; // after the job pool it writes autosaves with
    std::unique_ptr<UndoHistoryView> history;
    juce::Component::SafePointer<juce::DialogWindow> missingWindow;
    std::unique_ptr<juce::DocumentWindow> preferencesWindow;
    std::unique_ptr<Mappings> mapper;
    void tickMapper();
    struct Wake final : juce::AsyncUpdater
    {
        std::function<void()> onWake;
        void handleAsyncUpdate() override { onWake(); }
    };
    std::unique_ptr<juce::VBlankAttachment> mapperTick; // while MIDI Map has something to do
    Wake mapperWake;
    int mapperIdleFrames = 0;
    std::unique_ptr<MapOverlay> overlay;
    std::unique_ptr<MappingsView> mappingList;
    std::unique_ptr<FindIndex> finder;
    std::unique_ptr<FindView> findBox;
    juce::Component::SafePointer<juce::Component> flashed;
    std::unique_ptr<juce::KeyListener> shiftTabBack;
    ComputerKeyboardInput* computerKeyboard = nullptr;
    juce::String audioError; // why the audio device didn't open
    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<DetailView> detail; // after what it shows, so it goes first
    juce::Rectangle<int> detailDivider, browserDivider;
    int detailHeight = 0; // 0: the default share of the window
    int dragStartHeight = -1;
    int browserWidth = 0; // 0: the default
    int dragStartWidth = -1;
    juce::TabbedComponent tabs{juce::TabbedButtonBar::TabsAtTop};
    InfoView info;
    std::unique_ptr<StatusBar> status;
    bool showBrowser = false, showDetail = false, showInfo = true, showHistory = false;
    bool autoArm = true;
    AudioCallbackStats frames;
    juce::int64 paintStarted = 0;
};

} // namespace lattice::ui
