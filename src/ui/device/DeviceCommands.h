#pragma once

#include "ui/commands/CommandRegistry.h"
#include "ui/device/DeviceView.h"

namespace lattice::ui
{

/** The Device menu (docs/05-m4-ui.md, 4d): inserting each device type where you're looking, a
    sample in a new Sampler, and what the selected device's title bar does (on/off, fold, show all
    parameters, hot-swap, save as preset). Copy, Duplicate, Delete and Rename are the Edit
    commands, which act on the selected device while the Device view has the keys. */
void addDeviceCommands(CommandRegistry& registry, DeviceView& view);

} // namespace lattice::ui
