#pragma once

#include "control/surface/Surface.h"

namespace lattice::control::faders
{

// The grid's columns as faders, for Device and Mix modes on a device without encoders (a
// Launchpad): a column per control, its pads lit from the bottom up to the value (out from the
// middle for a bipolar one), and a press on a row sets the value there (the bottom row: the
// lowest, the top row: the highest).

/** The value (0..1 along the control) a press on a row sets. */
inline float positionOfRow(int row, int rows) noexcept
{
    return rows <= 1 ? 1.0f : static_cast<float>(rows - 1 - row) / static_cast<float>(rows - 1);
}

/** Lights one column for a value at `position` (0..1); unlit pads show the colour dimmed, so the
    column reads as a fader. */
inline void renderColumn(LedFrame& frame, const SurfaceCapabilities& caps, int column, float position, bool bipolar,
                         std::uint32_t colour, float dim)
{
    const int rows = caps.rows;
    const float half = rows <= 1 ? 0.5f : 0.5f / static_cast<float>(rows - 1);
    for (int r = 0; r < rows; ++r)
    {
        const float level = positionOfRow(r, rows);
        const bool lit = bipolar ? (position >= 0.5f ? level >= 0.5f - half && level <= position + half
                                                     : level <= 0.5f + half && level >= position - half)
                                 : level <= position + half;
        frame.pad(r, column, caps.columns) = LedState::solid(lit ? colour : dimmed(colour, dim));
    }
}

} // namespace lattice::control::faders
