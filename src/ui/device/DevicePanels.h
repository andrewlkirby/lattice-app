#pragma once

// The custom panels (docs/05-m4-ui.md, 4d, "Custom panels"), one factory per device type. Private
// to src/ui/device: everything else goes through createDeviceBody().

#include "ui/device/DeviceBody.h"

namespace lattice::ui::panels
{

using Factory = std::unique_ptr<DeviceBody> (*)(DeviceContext&, const juce::ValueTree&);

/** Loads a sample file into a Sampler, as one undo step (a new one unless newTransaction is false,
    for a caller that began its own). When the file says what its root note is (a WAV `smpl` or an
    AIFF `INST` chunk) and the Sampler follows the track's tuning, the root takes it, in the same
    step (M3 hands-on finding 6). The file is read in the background. */
void loadSampleInto(DeviceContext& context, juce::ValueTree sampler, const juce::File& file, bool newTransaction = true);

/** The panel factory for a device type, or nullptr (the type shows every parameter). */
Factory factoryFor(const juce::String& typeId);

std::unique_ptr<DeviceBody> synth(DeviceContext&, const juce::ValueTree&);
std::unique_ptr<DeviceBody> sampler(DeviceContext&, const juce::ValueTree&);
std::unique_ptr<DeviceBody> rack(DeviceContext&, const juce::ValueTree&);
std::unique_ptr<DeviceBody> eqEight(DeviceContext&, const juce::ValueTree&);
std::unique_ptr<DeviceBody> compressor(DeviceContext&, const juce::ValueTree&);
std::unique_ptr<DeviceBody> reverb(DeviceContext&, const juce::ValueTree&);
std::unique_ptr<DeviceBody> delay(DeviceContext&, const juce::ValueTree&);
std::unique_ptr<DeviceBody> autoFilter(DeviceContext&, const juce::ValueTree&);
std::unique_ptr<DeviceBody> saturator(DeviceContext&, const juce::ValueTree&);
std::unique_ptr<DeviceBody> chorus(DeviceContext&, const juce::ValueTree&);
std::unique_ptr<DeviceBody> utility(DeviceContext&, const juce::ValueTree&);
std::unique_ptr<DeviceBody> limiter(DeviceContext&, const juce::ValueTree&);
std::unique_ptr<DeviceBody> arpeggiator(DeviceContext&, const juce::ValueTree&);
std::unique_ptr<DeviceBody> chord(DeviceContext&, const juce::ValueTree&);

} // namespace lattice::ui::panels
