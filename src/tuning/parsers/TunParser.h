#pragma once

#include "tuning/TuningTable.h"

namespace lattice
{

/** AnaMark tuning files (.tun), versions 1 and 2 (http://www.mark-henning.de/eternity/tuningspecs.html):
    each note's frequency as cents above a base frequency. [Exact Tuning] (version 2: decimal cents,
    and its own BaseFreq) is used when the file has it, else [Tuning] (whole cents above
    8.1757989156 Hz). A note the file doesn't list keeps 12-TET's pitch, as the format says. The name
    is [Info]'s Name, if any. Keys and section names ignore case; ';' starts a comment. */
struct TunParser
{
    static TuningLoadResult parse(const juce::String& text, juce::String id, juce::String name, juce::String sourceDescription);
};

} // namespace lattice
