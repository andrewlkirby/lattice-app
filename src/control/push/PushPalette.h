#pragma once

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <cstdint>
#include <vector>

namespace lattice::control::push
{

/** Push's 128 LED colours, loaded with Lattice's own (docs/06-m5-controllers.md, "LEDs"): a light
    asks for a colour and gets the entry holding it, loading a free entry with it first if none
    does, so pads show the set's clip colours and the theme's exactly, not a fixed palette's
    nearest.

    Each entry has an RGB colour (for the RGB LEDs) and a white level (for the white ones): an RGB
    colour's entry gets the white level of its brightness, and a white LED asks for a level.
    Entry 0 is black and never changes. An entry a light shows is never reloaded (the device keeps
    each light's entry, and might look it up again), so a colour changes only on lights that ask
    for it; with every entry shown, a new colour gets the nearest one there is. Message thread. */
class Palette
{
public:
    static constexpr int size = 128;

    /** Forgets what the device holds: every entry is loaded again before it's used. */
    void reset();

    /** The entry for an RGB LED's colour (0: off). Any sysex to load it goes to `load`. */
    int forColour(std::uint32_t rgb, std::vector<juce::MidiMessage>& load);
    /** The entry for a white LED's level (0..255 of full; 0: off). */
    int forWhite(int level, std::vector<juce::MidiMessage>& load);

    /** A light shows the entry now / no longer shows it. */
    void use(int index) noexcept;
    void release(int index) noexcept;

    std::uint32_t colourOf(int index) const noexcept { return entries[static_cast<std::size_t>(index)].rgb; }
    int whiteOf(int index) const noexcept { return entries[static_cast<std::size_t>(index)].white; }
    int usesOf(int index) const noexcept { return entries[static_cast<std::size_t>(index)].uses; }

    /** The white level an RGB colour's entry gets: its brightness, as the device's own white
        palette scales it (full white is 128 there). */
    static int whiteFor(std::uint32_t rgb) noexcept;

private:
    struct Entry
    {
        std::uint32_t rgb = 0;
        int white = 0;
        int uses = 0;
        bool loaded = false;
        std::uint64_t lastUsed = 0;
    };
    int find(std::uint32_t rgb, int white, bool matchColour, std::vector<juce::MidiMessage>& load);

    std::array<Entry, size> entries{};
    std::uint64_t clock = 0;
};

} // namespace lattice::control::push
