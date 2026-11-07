#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace lattice::ui
{

/** Painting choices that depend on the renderer drawing a window (docs/06-m5-controllers.md, 5g,
    the quality pass). Message thread only. */
namespace painting
{
/** Whether keeping `c` as an image pays where it's drawn now. With the software renderer (the UI
    benchmark, snapshots, Linux and macOS) it does: copying an image beats drawing it again. With
    Direct2D (Windows) it doesn't: each redraw of a cached image opens and closes a drawing context
    of its own on the GPU, which cost far more than the drawing it saved (a mixer strip's meter,
    redrawn every frame, made it a fifth of the message thread's work under the UI stress). True
    for a component not on screen. */
bool cachedImagesPay(const juce::Component& c);

/** Buffers `c` to an image where that pays, and stops where it doesn't. Called when `c` may be on
    another window (parentHierarchyChanged). */
void bufferWhereItPays(juce::Component& c);

/** After a repaint() of `c`: lets its window paint at the next display refresh, even when the
    repaint came during that refresh's work (as the feed's tick, or an input handled in it, does).
    On Windows a repaint reaches JUCE's renderer only through WM_PAINT, which waits behind every
    posted message and so could miss the refresh, showing a frame late; this has it handled at once
    (it only marks what to paint: the painting stays on the refresh). Elsewhere it does nothing. */
void paintAtNextRefresh(juce::Component& c);
} // namespace painting

} // namespace lattice::ui
