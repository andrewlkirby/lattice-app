#pragma once

#include <array>
#include <vector>

namespace lattice::ui
{

/** Where things are in the piano roll (docs/05-m4-ui.md, 4c): time across, rows down, zoomed and
    scrolled. Plain arithmetic, tested without drawing.

    Rows are the 128 notes, highest at the top, or only some of them (folded to the notes in use).
    They're all one height, or, with log spacing, each row's centre sits in proportion to the log of
    its frequency, so the gaps between rows show the real intervals (in Colundi they differ). */
class PianoRollLayout
{
public:
    static constexpr double minPixelsPerQuarter = 2.0;
    static constexpr double maxPixelsPerQuarter = 2000.0;

    // --- Time ----------------------------------------------------------------------------------------
    double pixelsPerQuarter() const noexcept { return zoomX; }
    /** The time at the left edge, in quarter notes. */
    double scrollQuarters() const noexcept { return leftQuarters; }
    void setZoomX(double pixelsPerQuarter) noexcept;
    void setScrollQuarters(double quarters) noexcept;
    /** Zooms by a factor, keeping the time under x where it is. */
    void zoomXAround(double factor, double x) noexcept;
    double xFor(double quarters) const noexcept { return (quarters - leftQuarters) * zoomX; }
    double quartersAt(double x) const noexcept { return leftQuarters + x / zoomX; }

    // --- Rows ----------------------------------------------------------------------------------------
    /** The rows shown, any order (they're shown highest first). rowHeight: the height of a row, or
        with log spacing the average. hz: each note's frequency (128 of them) for log spacing, or
        nullptr for equal rows; log spacing falls back to equal rows if the shown rows' frequencies
        don't rise with their notes. */
    void setRows(std::vector<int> pitches, double rowHeight, const std::array<double, 128>* hz = nullptr);
    /** Every one of the 128 rows. */
    static std::vector<int> allRows();

    int numRows() const noexcept { return static_cast<int>(order.size()); }
    /** The row shown at a position from the top (0: the highest). */
    int pitchAtRow(int row) const noexcept { return order[static_cast<std::size_t>(row)]; }
    /** Where a note's row is shown (-1: not shown). */
    int rowOf(int pitch) const noexcept;
    bool isShown(int pitch) const noexcept { return rowOf(pitch) >= 0; }
    bool isLogSpaced() const noexcept { return logSpaced; }
    double averageRowHeight() const noexcept { return averageHeight; }

    /** The scroll from the top of the rows, in pixels. */
    double scrollY() const noexcept { return topY; }
    void setScrollY(double y, double viewHeight) noexcept;
    /** Every row, top to bottom, in pixels. */
    double contentHeight() const noexcept { return tops.empty() ? 0.0 : tops.back(); }
    /** A row's top and bottom in view (scrolled) coordinates. */
    double rowTop(int row) const noexcept { return tops[static_cast<std::size_t>(row)] - topY; }
    double rowBottom(int row) const noexcept { return tops[static_cast<std::size_t>(row) + 1] - topY; }
    /** A row's centre (with log spacing, where its frequency is). */
    double rowCentre(int row) const noexcept { return centres[static_cast<std::size_t>(row)] - topY; }
    /** The row at a view y (clamped to the first and last). */
    int rowAt(double y) const noexcept;
    /** The note shown at a view y. */
    int pitchAt(double y) const noexcept { return order.empty() ? -1 : pitchAtRow(rowAt(y)); }

private:
    double zoomX = 40.0;
    double leftQuarters = 0.0;
    std::vector<int> order;     // shown rows, highest first
    std::array<int, 128> rowIndex{};
    std::vector<double> tops;   // numRows + 1 edges, from 0
    std::vector<double> centres;
    double topY = 0.0;
    double averageHeight = 12.0;
    bool logSpaced = false;
};

} // namespace lattice::ui
