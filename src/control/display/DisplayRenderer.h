#pragma once

#include "control/surface/Display.h"
#include "ui/theme/Theme.h"

#include <juce_graphics/juce_graphics.h>

#include <vector>

namespace lattice::control
{

/** Draws a DisplayFrame (docs/06-m5-controllers.md, "Display rendering"), shared by Virtual Push
    and the hardware drivers: Push's display gets an image at its own size; Virtual Push draws
    straight into its component at whatever size it has.

    - A strip of labels for the upper display buttons along the top (or the title), one for the
      lower buttons along the bottom.
    - The cells in between, one per encoder, `across` to a row (Push: 8 in one row; a Virtual Push
      with 32 encoders: rows of 8 or 16): a caption (Mix: the track), the name, an arc for the
      value (out from the middle for a bipolar one), the value, and a meter where there is one.
    - While an encoder is touched its cell is shown large over its neighbours, with its range.

    Everything is sized from the bounds: nothing is a fixed pixel size. A display is dark in every
    theme (as a device's screen is); the theme gives its accent and meter colours. */
class DisplayRenderer
{
public:
    /** Where the parts go in bounds (plain arithmetic: Virtual Push hit-tests the cells). */
    struct Layout
    {
        juce::Rectangle<float> upper, lower; // the label strips
        std::vector<juce::Rectangle<float>> cells;
        int across = 1, rows = 0;
    };
    static Layout layoutFor(juce::Rectangle<float> bounds, int cells, int across);
    /** The height a display needs for its width: a Push-shaped cell (120 x 112) per encoder, and
        the two strips. */
    static float heightFor(float width, int cells, int across);

    static void draw(juce::Graphics& g, const DisplayFrame& frame, juce::Rectangle<float> bounds, int across,
                     const ui::Theme& theme);
    /** The frame drawn into an image of a device's size (Push: 960 x 160). */
    static juce::Image render(const DisplayFrame& frame, int width, int height, int across, const ui::Theme& theme);

    /** The display's own colours, for a theme. */
    struct Colours
    {
        juce::Colour background, text, dim, track, accent, selected;
    };
    static Colours coloursFor(const ui::Theme& theme);
};

} // namespace lattice::control
