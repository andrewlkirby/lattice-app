#pragma once

#include "app/debug/SessionUi.h"
#include "engine/SessionController.h"

#include <juce_gui_extra/juce_gui_extra.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace lattice::debugui
{

/** The clip launcher: track columns × scene rows, a scene launch column on the right, and a row of
    track stop buttons at the bottom. Painted in one component (no child per slot) and repainted
    only when something it shows changes.

    Left-click launches (and releasing the button releases, for Gate clips); right-click opens a menu
    (import MIDI, clip properties, duplicate, delete, scene and track options). */
class SessionGrid final : public juce::Component
{
public:
    static constexpr int headerHeight = 26;
    static constexpr int rowHeight = 30;
    static constexpr int stopRowHeight = 28;
    static constexpr int sceneColumnWidth = 150;

    SessionGrid(SessionController& session, Selection& selection);

    void setColumnWidth(int width);
    int columnWidth() const noexcept { return trackWidth; }
    int preferredWidth() const;
    int preferredHeight() const;

    /** Called on the UI tick: repaints if any slot state changed or something is blinking. */
    void poll();

    std::function<void()> onSelectionChanged;
    /** Asks the owner to choose a MIDI file for a slot. */
    std::function<void(int track, int scene)> onImportMidi;

    void paint(juce::Graphics& g) override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseUp(const juce::MouseEvent& e) override;
    void mouseDoubleClick(const juce::MouseEvent& e) override;

private:
    enum class Area : std::uint8_t
    {
        None,
        TrackHeader,
        Slot,
        Scene,
        SceneHeader,
        TrackStop,
        StopAll
    };

    struct Hit
    {
        Area area = Area::None;
        int track = -1;
        int scene = -1;
    };

    Hit findHit(juce::Point<int> p) const;
    juce::Rectangle<int> slotBounds(int track, int scene) const;
    void paintSlot(juce::Graphics& g, int track, int scene, SlotState state, bool blinkOn);
    void showSlotMenu(int track, int scene);
    void showSceneMenu(int scene);
    void showTrackMenu(int track);
    void showClipProperties(int track, int scene);
    void select(int track, int scene, bool sceneColumn);

    SessionController& session;
    Selection& selection;
    int trackWidth = 120;
    std::vector<SlotState> lastStates;
    std::vector<double> lastPositions;
    bool lastBlink = false;
    Hit pressed;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(SessionGrid)
};

} // namespace lattice::debugui
