#pragma once

#include "devices/framework/StudioDevice.h"

#include <vector>

namespace lattice
{

/** Lays out a device's parameter groups as controller pages for a surface with a given number of
    encoders (docs/04-m3-devices.md, "Parameter pages"). Nothing assumes 8: Push and Maschine ask
    for 8, a Virtual Push for 8, 16 or 32.

    - A group that fits on one page stays together.
    - Consecutive small groups share a page while they fit; the page is named after all of them
      ("Amp / Pan").
    - A group larger than a page is split into full pages in order ("Osc 1", "Osc 2", ...).
    - Every parameter appears on exactly one page, in group order, and no page has more than
      `encoders` parameters. */
class ParameterPager
{
public:
    static std::vector<ParameterGroup> pages(const std::vector<ParameterGroup>& groups, int encoders);

    /** The device's own layout for this encoder count if it has one, otherwise pages(). */
    static std::vector<ParameterGroup> pagesFor(const StudioDevice& device, int encoders);
};

} // namespace lattice
