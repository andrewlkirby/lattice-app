#pragma once

#include "control/surface/Surface.h"

#include <vector>

namespace lattice::control::presets
{

// What each candidate device has (docs/06-m5-controllers.md, "Hardware choice"), from the public
// specs and manuals. Every mode is tested against all of them on FakeSurface before any hardware
// arrives. The buttons are each device's as far as the roles go; they're unverified until the
// device itself is tested ("Hardware checklist").

SurfaceCapabilities push2();
SurfaceCapabilities push3();
SurfaceCapabilities launchpadProMk3();
SurfaceCapabilities apc64();
SurfaceCapabilities apc40Mk2();
SurfaceCapabilities maschineMk3();

/** Virtual Push (docs/06-m5-controllers.md, "Virtual Push"): every role, a grid of rows x columns
    (4 to 16 each), a scene button per row and a track button per column. */
SurfaceCapabilities virtualPush(int rows = 8, int columns = 8, int encoders = 8);
inline constexpr int minVirtualSize = 4;
inline constexpr int maxVirtualSize = 16;

/** A grid and nothing else: what "graceful degradation" is tested on. */
SurfaceCapabilities gridOnly(int rows, int columns);

/** Every device above, and Virtual Push at the sizes the tests use. */
std::vector<SurfaceCapabilities> allForTests();

} // namespace lattice::control::presets
