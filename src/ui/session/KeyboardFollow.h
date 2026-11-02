#pragma once

#include "control/ComputerKeyboardInput.h"
#include "core/model/LiveSet.h"

namespace lattice::ui
{

/** The first note of the pad page that holds a Drum Rack's lowest pad (pages of 16 from note 36,
    as the pad grid shows them), or -1 if the track's chain has no Drum Rack. */
int drumPadPageStart(const LiveSet& set, int track);

/** Makes the computer keyboard follow a Drum Rack (docs/05-m4-ui.md, "Found in the M3 hands-on
    checks", 5): on a Drum Rack's track, A plays the first pad on show; leaving the track puts the
    range back where it was. */
class KeyboardFollow
{
public:
    /** The selected track changed (or may have). */
    void trackSelected(const LiveSet& set, int track, ComputerKeyboardInput& keyboard);

private:
    bool onDrumRack = false;
    int baseBefore = 60;
};

} // namespace lattice::ui
