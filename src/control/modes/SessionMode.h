#pragma once

#include "control/surface/Mode.h"

#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace lattice::control
{

/** Session mode (docs/06-m5-controllers.md, "Session mode"): the grid shows a box of clip slots,
    columns = tracks (those the Session view shows: a folded group hides its tracks), rows =
    scenes, as large as the device's grid.

    - A pad launches its slot (and selects it); releasing it releases a Gate clip. With Shift it
      only selects. Held Delete or Duplicate deletes or duplicates the clip instead.
    - Scene buttons launch scenes. A device without them uses the grid's last column.
    - Track buttons (trackSelect, or the buttons under a display) select tracks; with Stop Clip,
      Mute or Solo held they stop, mute or solo it instead. A device without track buttons gets a
      row of track pads at the bottom while one of those is held.
    - The arrows move the selection by one (the box follows it), Shift+arrow by a page. Without
      arrows the box still follows the selection made on screen.
    - Holding the Session button shows the overview: a pad for each page of the set; pressing one
      moves the box there. */
class SessionMode final : public Mode
{
public:
    explicit SessionMode(ModeHost& host);

    ModeKind kind() const override { return ModeKind::session; }
    bool handle(const SurfaceInput& input) override;
    void render(LedFrame& frame) override;
    void selectionChanged() override;
    void enter() override;

    // --- The box --------------------------------------------------------------------------------
    /** The tracks the box can show, in order (one per Session view column). */
    std::vector<int> columnsOfSet() const;
    /** Grid columns that show tracks: all, or one fewer when the last launches scenes. */
    int trackColumns() const;
    /** Grid rows that show scenes: all, or one fewer while the track row shows. */
    int sceneRows() const;
    bool scenesOnGrid() const { return !caps().has(ButtonId{Role::scene, 0}); }
    /** Whether the bottom row shows tracks (Stop Clip, Mute or Solo held, with no track buttons). */
    bool trackRowShown() const;
    bool overviewShown() const;

    /** The box's top-left: a column of columnsOfSet(), and a scene. */
    int firstColumn() const noexcept { return column0; }
    int firstScene() const noexcept { return scene0; }
    /** Moves the box (clamped to the set). */
    void setOrigin(int column, int scene);

    /** The slot under a pad (track, scene), or nothing (outside the set, a scene or track pad). */
    std::optional<std::pair<int, int>> slotAt(int row, int column) const;

private:
    enum class Modifier
    {
        none,
        stopClip,
        mute,
        solo
    };
    Modifier heldModifier() const;
    /** The track a track button or track pad acts on, or -1. */
    int trackOfColumn(int gridColumn) const;
    void pressTrack(int track);
    void pressScene(int scene);
    void pressSlot(int padIndex, int track, int scene);
    void navigate(int dx, int dy);
    void showSelection();
    /** The overview's pages: across and down. */
    std::pair<int, int> pageCounts() const;

    LedState slotLed(int track, int scene) const;
    LedState sceneLed(int scene) const;
    LedState trackLed(int track) const;
    void renderOverview(LedFrame& frame) const;

    int column0 = 0, scene0 = 0;
    std::map<int, std::pair<int, int>> pressed; // pad index -> the slot it launched (for its release)
};

} // namespace lattice::control
