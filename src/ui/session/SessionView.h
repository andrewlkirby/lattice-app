#pragma once

#include "engine/SessionController.h"
#include "ui/commands/CommandRegistry.h"
#include "ui/feed/EngineStateFeed.h"
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

    It paints itself in one component, and repaints only what the engine state feed says changed
    (plus the playing slots' progress and the queued slots' blinking). Tracks and slots scroll;
    the headers, the stop row and the scene column stay in view. */
class SessionView final : public juce::Component,
                          private EngineStateFeed::Listener,
                          private SessionSelectionModel::Listener,
                          private juce::ValueTree::Listener,
                          private juce::AsyncUpdater,
                          private juce::ScrollBar::Listener
{
public:
    SessionView(SessionController& session, EngineStateFeed& feed, CommandRegistry& commands,
                SessionSelectionModel& selection);
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
            slot,
            launch,
            stop,
            sceneLaunch,
            scene,
            stopAll
        };
        Area area = Area::none;
        int column = -1; // into layout().columns()
        int scene = -1;
    };
    Hit hitAt(juce::Point<int> local) const;

    /** What a click, its release and a double-click do (the mouse handlers call these; tests too). */
    void press(const Hit& hit);
    void release();
    void doubleClick(const Hit& hit);
    /** Renames what's selected (a prompt), for the Rename command. */
    void renameSelection();

    /** Screen areas (local coordinates). */
    juce::Rectangle<int> headerArea() const noexcept { return headers; }
    juce::Rectangle<int> slotsArea() const noexcept { return slots; }
    juce::Rectangle<int> stopArea() const noexcept { return stops; }
    juce::Rectangle<int> sceneColumn() const noexcept { return scenes; }
    /** A slot's rectangle on screen (local coordinates; may be outside the slots area). */
    juce::Rectangle<int> slotOnScreen(int column, int scene) const;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseUp(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void engineStateChanged(std::uint32_t aspects) override;
    void selectionChanged(const SessionSelection& now) override;
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override;
    void scrollBarMoved(juce::ScrollBar*, double) override;

    void rebuild();
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
    SessionLayout grid;
    float widthZoom = 1.0f, heightZoom = 1.0f;
    juce::Point<int> scroll;
    juce::Rectangle<int> headers, slots, stops, scenes;
    juce::ScrollBar vertical{true}, horizontal{false};
    Hit pressed;
};

/** Asks for a line of text without a modal loop; calls onOk with it if the user confirms, and only
    if `owner` still exists then. */
void askForText(juce::Component& owner, const juce::String& title, const juce::String& initial,
                std::function<void(const juce::String&)> onOk);

} // namespace lattice::ui
