#include "ui/clip/PianoRollLayout.h"

#include <algorithm>
#include <cmath>
#include <numeric>

namespace lattice::ui
{

void PianoRollLayout::setZoomX(double pixelsPerQuarter) noexcept
{
    zoomX = std::clamp(pixelsPerQuarter, minPixelsPerQuarter, maxPixelsPerQuarter);
}

void PianoRollLayout::setScrollQuarters(double quarters) noexcept
{
    leftQuarters = std::max(0.0, quarters);
}

void PianoRollLayout::zoomXAround(double factor, double x) noexcept
{
    const double anchor = quartersAt(x);
    setZoomX(zoomX * factor);
    setScrollQuarters(anchor - x / zoomX);
}

std::vector<int> PianoRollLayout::allRows()
{
    std::vector<int> rows(128);
    std::iota(rows.begin(), rows.end(), 0);
    return rows;
}

void PianoRollLayout::setRows(std::vector<int> pitches, double rowHeight, const std::array<double, 128>* hz)
{
    pitches.erase(std::remove_if(pitches.begin(), pitches.end(), [](int p) { return p < 0 || p > 127; }), pitches.end());
    std::sort(pitches.begin(), pitches.end(), std::greater<>());
    pitches.erase(std::unique(pitches.begin(), pitches.end()), pitches.end());
    order = std::move(pitches);
    rowIndex.fill(-1);
    for (std::size_t r = 0; r < order.size(); ++r)
        rowIndex[static_cast<std::size_t>(order[r])] = static_cast<int>(r);

    averageHeight = std::max(1.0, rowHeight);
    const auto n = order.size();
    tops.assign(n + 1, 0.0);
    centres.assign(n, 0.0);

    // Log spacing needs frequencies that rise with the notes (highest first: falling down the rows).
    logSpaced = hz != nullptr && n >= 2;
    if (logSpaced)
        for (std::size_t r = 0; r < n; ++r)
        {
            const double f = (*hz)[static_cast<std::size_t>(order[r])];
            if (f <= 0.0 || (r > 0 && f >= (*hz)[static_cast<std::size_t>(order[r - 1])]))
                logSpaced = false;
        }

    if (!logSpaced)
    {
        for (std::size_t r = 0; r < n; ++r)
        {
            tops[r + 1] = static_cast<double>(r + 1) * averageHeight;
            centres[r] = (static_cast<double>(r) + 0.5) * averageHeight;
        }
        return;
    }

    // Centres at k * (log f_top - log f), with k making the centres averageHeight apart on average.
    // Edges halfway between centres; the outer rows are as tall as the gap to their neighbour.
    const auto logOf = [&](std::size_t r) { return std::log2((*hz)[static_cast<std::size_t>(order[r])]); };
    const double k = averageHeight * static_cast<double>(n - 1) / (logOf(0) - logOf(n - 1));
    const double firstGap = k * (logOf(0) - logOf(1));
    const double lastGap = k * (logOf(n - 2) - logOf(n - 1));
    for (std::size_t r = 0; r < n; ++r)
        centres[r] = firstGap * 0.5 + k * (logOf(0) - logOf(r));
    tops[0] = 0.0;
    for (std::size_t r = 1; r < n; ++r)
        tops[r] = (centres[r - 1] + centres[r]) * 0.5;
    tops[n] = centres[n - 1] + lastGap * 0.5;
}

int PianoRollLayout::rowOf(int pitch) const noexcept
{
    return pitch < 0 || pitch > 127 ? -1 : rowIndex[static_cast<std::size_t>(pitch)];
}

void PianoRollLayout::setScrollY(double y, double viewHeight) noexcept
{
    topY = std::clamp(y, 0.0, std::max(0.0, contentHeight() - viewHeight));
}

int PianoRollLayout::rowAt(double y) const noexcept
{
    if (order.empty())
        return 0;
    const double content = y + topY;
    // The last edge at or above y.
    const auto it = std::upper_bound(tops.begin() + 1, tops.end() - 1, content);
    return static_cast<int>(std::distance(tops.begin() + 1, it));
}

} // namespace lattice::ui
