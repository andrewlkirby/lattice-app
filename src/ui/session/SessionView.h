#pragma once

#include "engine/SessionController.h"
#include "tuning/TuningRegistry.h"
#include "ui/commands/CommandRegistry.h"
#include "ui/feed/EngineStateFeed.h"
#include "ui/session/MixerStrip.h"
#include "ui/session/SessionLayout.h"
#include "ui/session/SessionSelectionModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lattice::ui
{

/** The Session view (docs/05-m4-ui.md, 4b): track headers, the clip slots, the stop buttons and the
    scene column, as in Live.

    - A slot's launch button (the triangle at its left) launches its clip, and a Gate clip plays
      while it's held; an empty slot's button is its stop button, or its record button when the
      track is armed. A group's slot launches its row. Clicking a slot elsewhere selects it;
      double-clicking an empty one creates a clip.
    - Playing clips show their progress, queued ones blink, recording ones are red.
    - A group's header folds and unfolds it. The scene column launches scenes; the bottom row
      stops tracks, and Stop All every clip.
    - Right-clicking offers the commands for what was clicked.

    - The mixer sits at the bottom, a strip under each column and the master's under the scene
      column. It scrolls sideways with the tracks but never out of view (the M3 hands-on checks
      found the arm buttons and sends scrolled away below the grid). The mixer, and its sends,
      can be hidden.

    It paints itself in one component (the strips are children), and repaints only what the engine
    state feed says changed (plus the playing slots' progress and the queued slots' blinking).
    Tracks and slots scroll; the headers, the stop row, the mixer and the scene column stay in
    view. A change to the set's structure (tracks, returns, scenes, groups, folding) rebuilds the
    layout and the strips; anything else only repaints, so a fader being dragged stays put. */
class SessionView final : public juce::Component,
                          public juce::FileDragAndDropTarget,
                          private EngineStateFeed::Listener,
                          private SessionSelectionModel::Listener,
                          private juce::ValueTree::Listener,
                          private juce::AsyncUpdater,
                          private juce::ScrollBar::Listener
{
public:
    SessionView(SessionController& session, EngineStateFeed& feed, CommandRegistry& commands,
                SessionSelectionModel& selection, TuningRegistry& tunings);
    ~SessionView() override;

    const SessionLayout& layout() const noexcept { return grid; }
    /** Track width and scene height zoom (0.5 to 3). */
    void setZoom(float widthZoom, float heightZoom);
    juce::Point<int> scrollPosition() const noexcept { return scroll; }
    void setScrollPosition(juce::Point<int> position);
    /** Scrolls so the selection shows. */
    void revealSelection();

    /** What's under a point (local coordinates). */
    struct Hit
    {
        enum class Area
        {
            none,
            header,
            fold,
            tuning, // a MIDI track header's tuning badge
            slot,
            launch,
            stop,
            sceneLaunch,
            scene,
            stopAll,
            master // the corner above the scenes: the master's devices
        };
        Area area = Area::none;
        int column = -1; // into layout().columns()
        int scene = -1;
    };
    Hit hitAt(juce::Point<int> local) const;

    /** What a right-click selects before its menu opens: what was clicked, unless it's inside the
        selection, which then stays. */
    void selectForMenu(const Hit& hit);
    /** What a click, its release and a double-click do (the mouse handlers call these; tests too).
        Shift extends the selection to what was clicked. */
    void press(const Hit& hit, juce::ModifierKeys mods = {});
    void release();
    void doubleClick(const Hit& hit);

    /** Dragging clips: from the slot pressed to another; the selected clips go along, keeping their
        layout. copy (Ctrl): leaves the originals. Tracks that can't take clips are skipped. */
    void dragClipsTo(const Hit& target, bool copy);
    /** The tuning badge's area in a column's header (local coordinates; empty for groups and returns). */
    juce::Rectangle<int> tuningBadge(int column) const;
    /** The tunings a track can be given, for the badge's menu, and setting one (one undo step). */
    juce::PopupMenu tuningMenu(int track);
    /** Colours what's selected (clips, tracks or scenes) with a palette colour. */
    void colourSelection(int paletteIndex);
    /** Imports MIDI files at a slot (one per scene down from it). */
    void importMidiFiles(const juce::StringArray& files, const Hit& at);

    // juce::FileDragAndDropTarget: MIDI files from the OS file explorer.
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragMove(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;
    /** Called when a double-click asks to see the selected clip (on a clip, or making one). */
    std::function<void()> onOpenClip;
    /** Called when a click on a track's, a return's or the master's header asks to see its
        devices (the shell switches the detail view to the Device view, as Live does). */
    std::function<void()> onShowDevices;
    /** Renames what's selected (a prompt), for the Rename command. */
    void renameSelection();

    /** Applies pending document changes now (normally the message loop does; tests call it). */
    void flushUpdates() { handleUpdateNowIfNeeded(); }

    void setMixerVisible(bool visible);
    void setSendsVisible(bool visible);
    void setIOVisible(bool visible);
    bool isIOVisible() const noexcept { return showIO; }
    bool isMixerVisible() const noexcept { return showMixer; }
    bool areSendsVisible() const noexcept { return showSends; }
    /** The strip under a column (or nullptr), and the master's. */
    MixerStrip* stripForColumn(int column) const;
    MixerStrip* masterStrip() const noexcept { return master.get(); }

    /** Screen areas (local coordinates). */
    juce::Rectangle<int> headerArea() const noexcept { return headers; }
    juce::Rectangle<int> slotsArea() const noexcept { return slots; }
    juce::Rectangle<int> stopArea() const noexcept { return stops; }
    juce::Rectangle<int> sceneColumn() const noexcept { return scenes; }
    juce::Rectangle<int> mixerArea() const noexcept { return mixer; }
    /** A slot's rectangle on screen (local coordinates; may be outside the slots area). */
    juce::Rectangle<int> slotOnScreen(int column, int scene) const;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void engineStateChanged(std::uint32_t aspects) override;
    void selectionChanged(const SessionSelection& now) override;
    void deviceOwnerChanged(const DeviceOwner&) override { repaint(); }
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override
    {
        structureChanged = true;
        triggerAsyncUpdate();
    }
    void handleAsyncUpdate() override;
    void scrollBarMoved(juce::ScrollBar*, double) override;

    void rebuild();
    void rebuildStrips();
    void placeStrips();
    void updateScrollBars();
    bool blinkOn() const;
    void showMenu(const Hit& hit);
    void paintHeaders(juce::Graphics&);
    void paintSlots(juce::Graphics&);
    void paintSlot(juce::Graphics&, int column, int scene, juce::Rectangle<int> r);
    void paintStops(juce::Graphics&);
    void paintScenes(juce::Graphics&);
    int trackOf(int column) const;

    SessionController& session;
    EngineStateFeed& feed;
    CommandRegistry& commands;
    SessionSelectionModel& selection;
    TuningRegistry& tunings;
    SessionLayout grid;
    float widthZoom = 1.0f, heightZoom = 1.0f;
    juce::Point<int> scroll;
    juce::Rectangle<int> headers, slots, stops, scenes, mixer;
    juce::ScrollBar vertical{true}, horizontal{false};
    Hit pressed;
    Hit dragFrom, dragTo; // a clip drag in progress (dragFrom.area none: none)
    bool dragging = false;
    Hit dropTarget; // a file drag hovering over a slot
    bool structureChanged = true;
    bool showMixer = true, showSends = true, showIO = true;
    juce::Component stripArea; // clips the strips to the scrolling part
    std::vector<std::unique_ptr<MixerStrip>> strips; // one per column
    std::unique_ptr<MixerStrip> master;
};

/** Asks for a line of text without a modal loop; calls onOk with it if the user confirms, and only
    if `owner` still exists then. */
void askForText(juce::Component& owner, const juce::String& title, const juce::String& initial,
                std::function<void(const juce::String&)> onOk);

} // namespace lattice::ui
