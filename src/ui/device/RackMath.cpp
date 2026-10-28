#include "ui/device/RackMath.h"

#include <algorithm>
#include <cmath>

namespace lattice::ui::rackmath
{

MacroFit fitMacros(int count, int height, int knobHeight)
{
    if (count <= 0)
        return {0, 0};
    const int rows = std::clamp(knobHeight > 0 ? height / knobHeight : 1, 1, count);
    return {rows, (count + rows - 1) / rows};
}

float zoneX(int value, float left, float width, int minimum)
{
    const int steps = 128 - minimum;
    return left + static_cast<float>(value - minimum) / static_cast<float>(steps) * width;
}

int zoneValueAt(float x, float left, float width, int minimum)
{
    const int steps = 128 - minimum;
    const int v = minimum + static_cast<int>(std::lround((x - left) / std::max(1.0f, width) * static_cast<float>(steps)));
    return std::clamp(v, minimum, 128);
}

ZoneHandle zoneHandleAt(const Zone& z, float x, float y, float left, float width, float top, float height, float reach,
                        int minimum, float fadeShare)
{
    const float lx = zoneX(z.low, left, width, minimum);
    const float hx = zoneX(z.high + 1, left, width, minimum); // the right end of the highest step
    if (y < top || y > top + height || x < lx - reach || x > hx + reach)
        return ZoneHandle::none;
    if (y <= top + height * fadeShare)
    {
        const float fl = zoneX(z.low + z.fadeLow, left, width, minimum);
        const float fh = zoneX(z.high + 1 - z.fadeHigh, left, width, minimum);
        const float dl = std::abs(x - fl), dh = std::abs(x - fh);
        if (std::min(dl, dh) <= reach)
            return dl <= dh ? ZoneHandle::fadeLow : ZoneHandle::fadeHigh;
    }
    const float dl = std::abs(x - lx), dh = std::abs(x - hx);
    if (std::min(dl, dh) <= reach)
        return dl <= dh ? ZoneHandle::low : ZoneHandle::high;
    return x >= lx && x <= hx ? ZoneHandle::body : ZoneHandle::none;
}

Zone dragZone(const Zone& from, ZoneHandle handle, int delta, int minimum)
{
    Zone z = from;
    const auto clampFades = [&z] {
        const int span = z.high - z.low;
        z.fadeLow = std::clamp(z.fadeLow, 0, span);
        z.fadeHigh = std::clamp(z.fadeHigh, 0, span - z.fadeLow);
    };
    switch (handle)
    {
    case ZoneHandle::low:
        z.low = std::clamp(from.low + delta, minimum, from.high);
        clampFades();
        break;
    case ZoneHandle::high:
        z.high = std::clamp(from.high + delta, from.low, 127);
        clampFades();
        break;
    case ZoneHandle::fadeLow:
        z.fadeLow = std::clamp(from.fadeLow + delta, 0, from.high - from.low - from.fadeHigh);
        break;
    case ZoneHandle::fadeHigh:
        z.fadeHigh = std::clamp(from.fadeHigh - delta, 0, from.high - from.low - from.fadeLow);
        break;
    case ZoneHandle::body:
    {
        const int shift = std::clamp(delta, minimum - from.low, 127 - from.high);
        z.low = from.low + shift;
        z.high = from.high + shift;
        break;
    }
    case ZoneHandle::none:
        break;
    }
    return z;
}

PadPage fitPads(int width, int height, int minPad, int setting)
{
    if (setting == 32)
        return {8, 4};
    if (setting == 64)
        return {8, 8};
    if (setting == 16 || minPad <= 0)
        return {4, 4};
    // Fit: more rows when it's tall enough, then more columns when it's wide enough too.
    PadPage p;
    if (height >= 8 * minPad)
    {
        p.rows = 8;
        if (width >= 8 * minPad)
            p.columns = 8;
    }
    return p;
}

} // namespace lattice::ui::rackmath
