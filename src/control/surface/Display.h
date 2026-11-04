#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <vector>

namespace lattice::control
{

// What a device's display shows (docs/06-m5-controllers.md, "Display rendering"): structured
// data, one cell per encoder, drawn by DisplayRenderer at the device's size (or written as text
// lines on a text display). Modes fill it in; they never draw.

/** One encoder's cell: what it controls and where that is. */
struct DisplayCell
{
    juce::String caption{};   // a line above the name: the track's (Mix), empty for none
    juce::String name{};      // "Cutoff"
    juce::String value{};     // "1.20 kHz"
    float position = -1.0f;   // 0..1 along the control (skewed as its knob is); < 0: no graphic
    bool bipolar = false;     // drawn out from the middle (pan, a gain)
    float meter = -1.0f;      // a level beside it (Mix), as a mixer meter shows it (Meter::proportionForDb); < 0: none
    std::uint32_t colour = 0; // its accent (0xRRGGBB); 0: the display's own
    bool selected = false;    // the selected track (Mix)
    juce::String detail{};    // the detail view's extra line: the range ("20 Hz - 20.0 kHz")

    bool operator==(const DisplayCell&) const = default;
};

/** The label over (or under) one of the buttons along the display. */
struct DisplayButton
{
    juce::String label{};
    std::uint32_t colour = 0; // 0: the display's own
    bool selected = false;

    bool operator==(const DisplayButton&) const = default;
};

struct DisplayFrame
{
    juce::String title{};               // shown where the upper labels go when there are none
    std::vector<DisplayButton> upper{}; // one per upper display button
    std::vector<DisplayCell> cells{};   // one per encoder (empty cells: nothing on that encoder)
    std::vector<DisplayButton> lower{}; // one per lower display button
    int detail = -1;                    // the touched encoder, shown large (-1: none)
    juce::String message{};             // in the middle when no cell has anything ("No devices")

    bool operator==(const DisplayFrame&) const = default;
};

/** A frame as a text display shows it (lines x characters, as an APC64's): the touched
    encoder's name and value; otherwise the title, the message or the cells, one a line. */
std::vector<juce::String> displayText(const DisplayFrame& frame, int lines, int characters);

} // namespace lattice::control
