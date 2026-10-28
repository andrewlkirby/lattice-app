#include "ui/device/DeviceCommands.h"

#include "devices/ChainEditing.h"
#include "devices/racks/Rack.h"

namespace lattice::ui
{

void addDeviceCommands(CommandRegistry& registry, DeviceView& view)
{
    auto& context = view.deviceContext();
    const auto hasSelection = [&view, &context] {
        return view.selectedDevice().isValid() && view.selectedDevice().isAChildOf(context.set().state());
    };

    // One Insert command per device type, so each is in the menu, in Find, and can have a key.
    for (const auto& type : context.registry().types())
    {
        if (type.hidden)
            continue;
        registry.add({.id = "device.insert." + type.id.fromFirstOccurrenceOf("lattice.", false, false),
                      .name = "Insert " + type.name,
                      .category = "Device",
                      .description = "Inserts " + type.name + " where you're working in the Device view: after the selected device, "
                                                               "or into the chain you clicked last (a rack's chain too).",
                      .perform = [&view, id = type.id] { view.insertHere(drag::device(id)); },
                      .isEnabled =
                          [&view, &context, id = type.id] {
                              const auto chain = view.insertionChain();
                              return chain.isValid() &&
                                     chains::place(chain, id, view.insertionIndex(), context.registry()).allowed;
                          }});
    }
    registry.add({.id = "device.insertSample",
                  .name = "Insert a Sample...",
                  .category = "Device",
                  .description = "Asks for a sample and plays it in a new Sampler, where you're working in the Device view.",
                  .perform =
                      [&view, &context] {
                          if (!context.chooseFile)
                              return;
                          juce::Component::SafePointer<DeviceView> safe(&view);
                          context.chooseFile("Load a sample into a new Sampler", sampleFilePatterns, [safe](const juce::File& f) {
                              if (safe != nullptr)
                                  safe->insertHere(drag::sample(f));
                          });
                      },
                  .isEnabled = [&view] { return view.insertionChain().isValid(); }});

    // What the selected device's title bar does.
    registry.add({.id = "device.onOff",
                  .name = "Device On",
                  .category = "Device",
                  .description = "Turns the selected device on or off.",
                  .perform =
                      [&view, &context] {
                          auto d = view.selectedDevice();
                          context.undo().beginNewTransaction("Device on/off");
                          context.set().setDeviceEnabled(d, !static_cast<bool>(d.getProperty(ids::enabled, true)));
                      },
                  .isEnabled = hasSelection,
                  .isTicked = [&view] { return static_cast<bool>(view.selectedDevice().getProperty(ids::enabled, true)); }});
    registry.add({.id = "device.fold",
                  .name = "Fold Device",
                  .category = "Device",
                  .description = "Folds the selected device to a narrow strip, or unfolds it.",
                  .perform = [&view] { LiveSet::setDeviceFolded(view.selectedDevice(), !view.selectedDevice().getProperty(ids::folded, false)); },
                  .isEnabled = hasSelection,
                  .isTicked = [&view] { return static_cast<bool>(view.selectedDevice().getProperty(ids::folded, false)); }});
    registry.add({.id = "device.showAll",
                  .name = "Show All Parameters",
                  .category = "Device",
                  .description = "Shows every parameter of the selected device instead of its own panel.",
                  .perform = [&view] { LiveSet::setDeviceShowsAll(view.selectedDevice(), !view.selectedDevice().getProperty(ids::showAll, false)); },
                  .isEnabled =
                      [&view, hasSelection] {
                          const auto type = view.selectedDevice()[ids::deviceType].toString();
                          return hasSelection() && hasOwnPanel(type) && !RackDevice::isRackType(type);
                      },
                  .isTicked = [&view] { return static_cast<bool>(view.selectedDevice().getProperty(ids::showAll, false)); }});
    registry.add({.id = "device.hotSwap",
                  .name = "Hot-Swap Device",
                  .category = "Device",
                  .description = "The next device or preset you insert replaces the selected device (Esc ends it).",
                  .perform =
                      [&view] {
                          const auto d = view.selectedDevice();
                          view.setHotSwapTarget(view.hotSwapTarget() == d ? juce::ValueTree() : d);
                      },
                  .isEnabled = hasSelection,
                  .isTicked = [&view] { return view.hotSwapTarget().isValid() && view.hotSwapTarget() == view.selectedDevice(); }});
    registry.add({.id = "device.savePreset",
                  .name = "Save Device as Preset...",
                  .category = "Device",
                  .description = "Saves the selected device, with everything in it, as a preset.",
                  .perform =
                      [&view] {
                          if (auto* f = view.frameFor(view.selectedDevice()))
                              if (f->saveButton().onClick)
                                  f->saveButton().onClick();
                      },
                  .isEnabled = hasSelection});
}

} // namespace lattice::ui
