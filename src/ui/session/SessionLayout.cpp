#include "ui/session/SessionLayout.h"

namespace lattice::ui
{

SessionLayout::Metrics SessionLayout::metricsFor(float controlHeight, float widthZoom, float heightZoom)
{
    widthZoom = juce::jlimit(0.5f, 3.0f, widthZoom);
    heightZoom = juce::jlimit(0.5f, 3.0f, heightZoom);
    Metrics metrics;
    metrics.trackWidth = controlHeight * 5.5f * widthZoom;
    metrics.returnWidth = controlHeight * 5.5f * widthZoom;
    metrics.rowHeight = controlHeight * heightZoom;
    metrics.headerHeight = controlHeight * 1.1f;
    metrics.stopRowHeight = controlHeight;
    metrics.sceneWidth = controlHeight * 6.0f;
    return metrics;
}

void SessionLayout::build(const LiveSet& set, const Metrics& metrics)
{
    m = metrics;
    cols.clear();
    scenes = set.numScenes();
    int x = 0;
    const int trackWidth = juce::roundToInt(m.trackWidth);
    for (int t = 0; t < set.numTracks();)
    {
        Column c;
        c.index = t;
        c.x = x;
        c.width = trackWidth;
        c.depth = set.groupDepth(t);
        c.isGroup = set.isGroupTrack(t);
        c.folded = c.isGroup && static_cast<bool>(set.track(t)[ids::folded]);
        cols.push_back(c);
        x += trackWidth;
        t = c.folded ? set.blockEnd(t) : t + 1; // a folded group hides what's in it
    }
    const int returnWidth = juce::roundToInt(m.returnWidth);
    for (int r = 0; r < set.numReturns(); ++r)
    {
        Column c;
        c.kind = Column::Kind::returnTrack;
        c.index = r;
        c.x = x;
        c.width = returnWidth;
        cols.push_back(c);
        x += returnWidth;
    }
    width = x;
}

int SessionLayout::columnAt(int x) const
{
    for (std::size_t i = 0; i < cols.size(); ++i)
        if (x >= cols[i].x && x < cols[i].x + cols[i].width)
            return static_cast<int>(i);
    return -1;
}

int SessionLayout::columnOfTrack(int track) const
{
    for (std::size_t i = 0; i < cols.size(); ++i)
        if (cols[i].kind == Column::Kind::track && cols[i].index == track)
            return static_cast<int>(i);
    return -1;
}

int SessionLayout::columnOfReturn(int returnIndex) const
{
    for (std::size_t i = 0; i < cols.size(); ++i)
        if (cols[i].kind == Column::Kind::returnTrack && cols[i].index == returnIndex)
            return static_cast<int>(i);
    return -1;
}

int SessionLayout::sceneAt(int y) const
{
    if (y < 0 || rowHeight() <= 0)
        return -1;
    const int s = y / rowHeight();
    return s < scenes ? s : -1;
}

juce::Rectangle<int> SessionLayout::slotBounds(int column, int scene) const
{
    if (column < 0 || column >= static_cast<int>(cols.size()) || scene < 0 || scene >= scenes)
        return {};
    const auto& c = cols[static_cast<std::size_t>(column)];
    return {c.x, scene * rowHeight(), c.width, rowHeight()};
}

juce::Rectangle<int> SessionLayout::launchButton(juce::Rectangle<int> slot) const
{
    return slot.withWidth(juce::jmin(slot.getWidth(), slot.getHeight()));
}

juce::Rectangle<int> SessionLayout::foldButton(int column) const
{
    if (column < 0 || column >= static_cast<int>(cols.size()))
        return {};
    const auto& c = cols[static_cast<std::size_t>(column)];
    const int size = juce::roundToInt(m.headerHeight);
    return {c.x, 0, juce::jmin(size, c.width), size};
}

} // namespace lattice::ui
