#include "ui/session/KeyboardFollow.h"

#include "devices/racks/Rack.h"

#include <cmath>

namespace lattice::ui
{

int drumPadPageStart(const LiveSet& set, int track)
{
    if (track < 0 || track >= set.numTracks())
        return -1;
    for (const auto& device : LiveSet::chainOf(set.track(track)))
    {
        if (device[ids::deviceType].toString() != RackDevice::drumRackId)
            continue;
        int lowest = 128;
        for (const auto& pad : device.getChildWithName(ids::rackChains))
            lowest = juce::jmin(lowest, static_cast<int>(pad.getProperty(ids::receiveNote, 128)));
        const int note = lowest < 128 ? lowest : 36;
        return juce::jlimit(0, 112, 36 + 16 * static_cast<int>(std::floor((note - 36) / 16.0)));
    }
    return -1;
}

void KeyboardFollow::trackSelected(const LiveSet& set, int track, ComputerKeyboardInput& keyboard)
{
    if (const int page = drumPadPageStart(set, track); page >= 0)
    {
        if (!onDrumRack)
            baseBefore = keyboard.getBaseNote();
        onDrumRack = true;
        keyboard.setBaseNote(page);
    }
    else if (onDrumRack)
    {
        onDrumRack = false;
        keyboard.setBaseNote(baseBefore);
    }
}

} // namespace lattice::ui
