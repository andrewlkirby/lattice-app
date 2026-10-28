#pragma once

// The arithmetic of the racks' panels (docs/05-m4-ui.md, 4d; Tests, item 1: "fitting macros into
// rows", "zone-editor handle math"), kept apart from drawing so tests can check it.

namespace lattice::ui::rackmath
{

/** How many rows and columns of macro knobs fit: as many rows as the height takes (at least
    one), the knobs filling them in order, so 64 macros wrap into as many rows as there's room for. */
struct MacroFit
{
    int rows = 0;
    int columns = 0;
};
MacroFit fitMacros(int count, int height, int knobHeight);

/** A key or velocity zone: the range it takes, and the fades at its ends (in notes or velocity
    steps, inside the range). */
struct Zone
{
    int low = 0;
    int high = 127;
    int fadeLow = 0;
    int fadeHigh = 0;

    bool operator==(const Zone&) const = default;
};

enum class ZoneHandle
{
    none,
    low,      // the left end
    high,     // the right end
    fadeLow,  // where the fade in ends
    fadeHigh, // where the fade out starts
    body      // the whole zone (moves it)
};

/** x (from `left`, `width` wide) of a value from `minimum` to 127, at the left edge of its step. */
float zoneX(int value, float left, float width, int minimum = 0);
/** The value under x (rounded to the nearest step edge). */
int zoneValueAt(float x, float left, float width, int minimum = 0);

/** What's under a point of a zone's bar: the fade handles along the top (the upper `fadeShare` of
    its height), the ends anywhere within `reach` pixels, otherwise the body; none outside it. */
ZoneHandle zoneHandleAt(const Zone& zone, float x, float y, float left, float width, float top, float height, float reach,
                        int minimum = 0, float fadeShare = 0.4f);

/** The zone after dragging a handle by `delta` steps from where it was (`from`): the ends stay in
    order and in range, and the fades stay inside the zone. */
Zone dragZone(const Zone& from, ZoneHandle handle, int delta, int minimum = 0);

/** The Drum Rack's pad page: `setting` pads (16, 32 or 64; 0 fits the space), as rows and columns of
    pads at least minPad pixels square. 4×4 by default; it grows to 4 columns × 8 rows, then 8×8,
    as the space allows. */
struct PadPage
{
    int rows = 4;
    int columns = 4;
    int size() const noexcept { return rows * columns; }
};
PadPage fitPads(int width, int height, int minPad, int setting);

} // namespace lattice::ui::rackmath
