#pragma once

#include "control/surface/BrowseHost.h"
#include "control/surface/Mode.h"

#include <array>

namespace lattice::control
{

/** Browse mode (docs/06-m5-controllers.md, "Browse mode"): the window's browser on the display,
    the encoders scrolling its lists (BrowseHost). The screen's browser follows, and selecting a
    row plays its preview on the preview bus, as clicking it does.

    - The display shows the sources (a column, more on a wide display) and the rows of the one
      chosen, each list keeping its selection in view. The encoders over the sources choose one;
      the others move along the rows, a row every few steps (with Shift a page).
    - The upper display buttons are the categories (Instruments, Drums...); the lower ones Load,
      Open (a folder, or a device's presets), Back (up a folder), Preview on or off, and Hot-swap
      (the next Load replaces the selected device).
    - A device without encoders shows the rows on its grid, a row of pads each: a press selects
      (and previews) it, its last pad loads it; the arrows move along the rows (up, down, with
      Shift a page) and in and out of folders (right, left); the scene buttons choose the
      categories. With encoders, the arrows stay the pad mode's, as in Device and Mix. */
class BrowseMode final : public Mode, private BrowseHost::Listener
{
public:
    BrowseMode(ModeHost& host, BrowseHost& browser);
    ~BrowseMode() override;

    ModeKind kind() const override { return ModeKind::browse; }
    bool handle(const SurfaceInput& input) override;
    void render(LedFrame& frame) override;
    void describe(SurfaceText& text) override;
    void display(DisplayFrame& frame) override;
    std::uint32_t aspects() const override { return ui::EngineStateFeed::transportAspect; }

    /** How many of the display's columns the sources take. */
    int sourceColumns() const;
    /** Moves the selected row (clamped), or the source. */
    void moveRow(int by);
    void moveSource(int by);
    int sourceIndex() const;

    /** What the lower buttons do. */
    enum class Action
    {
        load,
        open,
        back,
        preview,
        hotSwap
    };
    static const std::vector<std::pair<Action, juce::String>>& actions();
    void perform(Action action);

    /** Encoder steps to a row. */
    static constexpr int stepsPerRow = 10;
    /** Rows a page (Shift). */
    static constexpr int pageRows = 10;

private:
    bool padsOwned() const { return caps().encoders <= 0; }
    /** The first row the grid shows (without encoders). */
    int firstGridRow() const;
    void chooseCategory(int index);
    void browserChanged() override { host.requestRefresh(); }

    BrowseHost& browse;
    std::array<int, 2> pending{}; // steps short of a move: sources, rows
};

} // namespace lattice::control
