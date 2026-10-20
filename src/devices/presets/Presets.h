#pragma once

#include "devices/framework/DeviceRegistry.h"

#include <juce_data_structures/juce_data_structures.h>

#include <vector>

namespace lattice::presets
{

/** Device presets (docs/04-m3-devices.md, "3f"): `.lpreset` XML files holding one device's tree
    (a whole rack, with its chains, macros and mappings, is one device).

        PRESET   formatVersion (the preset file's), setFormatVersion (the DEVICE tree's: LiveSet's
         │       format), name, deviceType
         └─ DEVICE ...   as in a set, IDs included (inserting a preset gives it new ones)

    A preset leaves out what only means something in the set it came from: sidechain sources
    (track IDs). Sample files are kept as absolute paths. */

inline constexpr int formatVersion = 1;
inline constexpr const char* fileExtension = ".lpreset";
inline constexpr const char* fileWildcard = "*.lpreset";

/** A preset read back: the DEVICE tree to insert (LiveSet::insertDeviceCopy) and its name. */
struct Preset
{
    juce::String name;
    juce::ValueTree device;
};

/** The PRESET tree for a device. */
juce::ValueTree make(const juce::ValueTree& device, const juce::String& name);
juce::String toXmlString(const juce::ValueTree& device, const juce::String& name);
juce::Result write(const juce::ValueTree& device, const juce::String& name, const juce::File& file);

/** Reads a preset; fails on anything that isn't one, or on a newer format than this build knows. */
juce::Result parse(const juce::String& xml, Preset& out);
juce::Result read(const juce::File& file, Preset& out);

/** One preset in the library. */
struct PresetInfo
{
    juce::String name;
    juce::String deviceType;
    bool factory = false;
    juce::File file;              // a user preset
    juce::String factoryResource; // a factory preset: its BinaryData resource name
};

/** The factory presets (built in) and the user's (`<userdata>/Library/Presets/<Device>/`).
    Message thread. */
class PresetLibrary
{
public:
    /** userRoot: `<userdata>/Library/Presets`. Scans it once; rescan() after changes elsewhere. */
    PresetLibrary(juce::File userRoot, const DeviceRegistry& registry);

    static juce::File defaultUserRoot(const juce::File& userData)
    {
        return userData.getChildFile("Library").getChildFile("Presets");
    }

    void rescan();

    /** Every preset, factory ones first, then by device type and name. */
    const std::vector<PresetInfo>& all() const noexcept { return presets; }
    /** The presets for one device type, in the same order. */
    std::vector<PresetInfo> presetsFor(const juce::String& deviceType) const;

    juce::Result load(const PresetInfo& info, Preset& out) const;

    /** Where a device type's user presets go: the device's name as a folder. */
    juce::File userFolderFor(const juce::String& deviceType) const;

    /** Saves a device as a user preset named `name`, replacing a user preset of that name for the
        same device type; returns the file in `written`. */
    juce::Result saveUserPreset(const juce::ValueTree& device, const juce::String& name, juce::File& written);

    const juce::File& userRoot() const noexcept { return root; }

private:
    void scanFactory();
    void scanUser();
    void sort();

    juce::File root;
    const DeviceRegistry& registry;
    std::vector<PresetInfo> presets;
};

} // namespace lattice::presets
