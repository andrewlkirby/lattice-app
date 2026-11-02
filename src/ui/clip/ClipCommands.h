#pragma once

#include "ui/clip/PianoRoll.h"
#include "ui/commands/CommandRegistry.h"

namespace lattice::ui
{

/** The Clip view's commands (docs/05-m4-ui.md, 4c), category "Clip": the piano roll's modes (draw
    mode B, the grid's Ctrl+1 to 5 as in Live, fold, log-spaced rows, follow), the note tools
    (Ctrl+E split, Ctrl+J join, Ctrl+U quantize, legato, reverse, invert, stretch, transpose), the
    loop tools and the highlights. They act on the clip the Clip view shows. The arrow keys move
    and resize notes while the piano roll has the keys; the commands they run have no shortcuts of
    their own, since the arrows are the Session view's otherwise. */
void addClipCommands(CommandRegistry& registry, PianoRoll& roll);

} // namespace lattice::ui
