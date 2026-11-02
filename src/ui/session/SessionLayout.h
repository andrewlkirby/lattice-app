#pragma once

#include "core/model/LiveSet.h"

#include <juce_graphics/juce_graphics.h>

#include <vector>

namespace lattice::ui
{

/** Where everything in the Session view goes (docs/05-m4-ui.md, 4b), as plain arithmetic so tests
    can check it without drawing: one column per visible track (a folded group hides what's in
    it), then one per return track; one row per scene. All in content coordinates (before
    scrolling), in the theme's logical pixels times the zoom.

        | track headers ................ | return headers |
        | clip slots ................... |                |
        | stop buttons ................. |                |
        (the scene column and the master sit to the right, outside the scrolling area) */
class SessionLayout
{
public:
    struct Metrics
    {
        float trackWidth = 0.0f;  // a track's column
        float returnWidth = 0.0f; // a return's column
        float rowHeight = 0.0f;   // a scene's row
        float headerHeight = 0.0f;
        float stopRowHeight = 0.0f;
        float sceneWidth = 0.0f; // the scene column, outside the scrolling area
    };

    /** The sizes for a theme size unit (controlHeight) and the two zoom factors (track width and
        scene height, 0.5 to 3). */
    static Metrics metricsFor(float controlHeight, float widthZoom = 1.0f, float heightZoom = 1.0f);

    struct Column
    {
        enum class Kind
        {
            track,
            returnTrack
        };
        Kind kind = Kind::track;
        int index = 0; // the track's or return's index in the set
        int x = 0;
        int width = 0;
        int depth = 0; // how many groups it's in
        bool isGroup = false;
        bool folded = false;
    };

    void build(const LiveSet& set, const Metrics& metrics);

    const Metrics& metrics() const noexcept { return m; }
    const std::vector<Column>& columns() const noexcept { return cols; }
    int numScenes() const noexcept { return scenes; }
    /** The scrolling area's width (every column) and the slots' height (every scene). */
    int contentWidth() const noexcept { return width; }
    int rowsHeight() const noexcept { return scenes * rowHeight(); }
    int rowHeight() const noexcept { return juce::roundToInt(m.rowHeight); }

    /** The column under x (content coordinates), or -1. */
    int columnAt(int x) const;
    /** The column showing a track, or -1 if it's hidden in a folded group. */
    int columnOfTrack(int track) const;
    int columnOfReturn(int returnIndex) const;
    /** The scene row under y (in the slots area, content coordinates), or -1. */
    int sceneAt(int y) const;

    juce::Rectangle<int> slotBounds(int column, int scene) const;
    /** The launch button inside a slot: a square at its left. */
    juce::Rectangle<int> launchButton(juce::Rectangle<int> slot) const;
    /** A group header's fold button. */
    juce::Rectangle<int> foldButton(int column) const;

private:
    Metrics m;
    std::vector<Column> cols;
    int scenes = 0;
    int width = 0;
};

} // namespace lattice::ui
