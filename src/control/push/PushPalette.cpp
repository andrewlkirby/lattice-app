#include "control/push/PushPalette.h"

#include "control/push/PushProtocol.h"

#include <algorithm>
#include <limits>

namespace lattice::control::push
{

void Palette::reset()
{
    entries = {};
    entries[0].loaded = true; // black, as the device has it
    clock = 0;
}

int Palette::whiteFor(std::uint32_t rgb) noexcept
{
    const auto peak = std::max({(rgb >> 16) & 0xffu, (rgb >> 8) & 0xffu, rgb & 0xffu});
    return static_cast<int>((peak * 128u + 127u) / 255u);
}

int Palette::forColour(std::uint32_t rgb, std::vector<juce::MidiMessage>& load)
{
    rgb &= 0xffffffu;
    if (rgb == 0)
        return 0;
    return find(rgb, whiteFor(rgb), true, load);
}

int Palette::forWhite(int level, std::vector<juce::MidiMessage>& load)
{
    level = std::clamp(level, 0, 255);
    if (level == 0)
        return 0;
    // A grey for the RGB side, should an RGB light ever share it.
    const auto v = static_cast<std::uint32_t>(std::min(255, level * 2));
    return find((v << 16) | (v << 8) | v, level, false, load);
}

int Palette::find(std::uint32_t rgb, int white, bool matchColour, std::vector<juce::MidiMessage>& load)
{
    ++clock;
    // Already there.
    for (int i = 1; i < size; ++i)
    {
        auto& e = entries[static_cast<std::size_t>(i)];
        if (e.loaded && e.white == white && (!matchColour || e.rgb == rgb))
        {
            e.lastUsed = clock;
            return i;
        }
    }
    // A free entry: one never loaded, or the one shown least recently.
    int free = -1;
    for (int i = 1; i < size; ++i)
    {
        const auto& e = entries[static_cast<std::size_t>(i)];
        if (e.uses > 0)
            continue;
        if (!e.loaded)
        {
            free = i;
            break;
        }
        if (free < 0 || e.lastUsed < entries[static_cast<std::size_t>(free)].lastUsed)
            free = i;
    }
    if (free > 0)
    {
        auto& e = entries[static_cast<std::size_t>(free)];
        e.rgb = rgb;
        e.white = white;
        e.loaded = true;
        e.lastUsed = clock;
        load.push_back(setPaletteEntry(free, rgb, white));
        return free;
    }
    // Every entry is lit: the nearest there is.
    int best = 0;
    long bestDistance = std::numeric_limits<long>::max();
    for (int i = 1; i < size; ++i)
    {
        const auto& e = entries[static_cast<std::size_t>(i)];
        const auto channel = [](std::uint32_t c, int shift) { return static_cast<long>((c >> shift) & 0xffu); };
        const long d = matchColour ? (channel(e.rgb, 16) - channel(rgb, 16)) * (channel(e.rgb, 16) - channel(rgb, 16)) * 3 +
                                         (channel(e.rgb, 8) - channel(rgb, 8)) * (channel(e.rgb, 8) - channel(rgb, 8)) * 4 +
                                         (channel(e.rgb, 0) - channel(rgb, 0)) * (channel(e.rgb, 0) - channel(rgb, 0)) * 2
                                   : static_cast<long>(e.white - white) * (e.white - white);
        if (d < bestDistance)
        {
            bestDistance = d;
            best = i;
        }
    }
    return best;
}

void Palette::use(int index) noexcept
{
    if (index > 0 && index < size)
        ++entries[static_cast<std::size_t>(index)].uses;
}

void Palette::release(int index) noexcept
{
    if (index > 0 && index < size && entries[static_cast<std::size_t>(index)].uses > 0)
        --entries[static_cast<std::size_t>(index)].uses;
}

} // namespace lattice::control::push
