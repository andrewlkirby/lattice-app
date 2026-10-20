#include "devices/presets/Presets.h"

#include "core/model/LiveSet.h"

#include <LatticeBinaryData.h>

#include <algorithm>

namespace lattice::presets
{

namespace
{
const juce::Identifier presetTag{"PRESET"};
const juce::Identifier setFormatVersion{"setFormatVersion"};
const juce::Identifier presetName{"name"};

/** Removes what only means something in the set a device came from. */
void stripSetLinks(juce::ValueTree node)
{
    if (node.hasType(ids::deviceState))
    {
        node.removeProperty(ids::sidechainTrack, nullptr);
        node.removeProperty(ids::sidechainPreFx, nullptr);
    }
    for (auto child : node)
        stripSetLinks(child);
}

juce::String fileNameFor(const juce::String& name)
{
    auto legal = juce::File::createLegalFileName(name.trim());
    if (legal.isEmpty())
        legal = "Preset";
    return legal + fileExtension;
}
} // namespace

juce::ValueTree make(const juce::ValueTree& device, const juce::String& name)
{
    jassert(device.hasType(ids::device));
    juce::ValueTree preset(presetTag);
    preset.setProperty(ids::formatVersion, formatVersion, nullptr);
    preset.setProperty(setFormatVersion, LiveSet::currentFormatVersion, nullptr);
    preset.setProperty(presetName, name, nullptr);
    preset.setProperty(ids::deviceType, device[ids::deviceType], nullptr);
    auto copy = device.createCopy();
    stripSetLinks(copy);
    preset.appendChild(copy, nullptr);
    return preset;
}

juce::String toXmlString(const juce::ValueTree& device, const juce::String& name)
{
    return make(device, name).toXmlString();
}

juce::Result write(const juce::ValueTree& device, const juce::String& name, const juce::File& file)
{
    if (const auto made = file.getParentDirectory().createDirectory(); made.failed())
        return made;
    // Write next to the target, then swap it in: a failed write never leaves half a preset.
    juce::TemporaryFile temp(file);
    if (!temp.getFile().replaceWithText(toXmlString(device, name), false, false, "\n"))
        return juce::Result::fail("Could not write " + file.getFullPathName());
    if (!temp.overwriteTargetFileWithTemporary())
        return juce::Result::fail("Could not replace " + file.getFullPathName());
    return juce::Result::ok();
}

juce::Result parse(const juce::String& xml, Preset& out)
{
    const auto tree = juce::ValueTree::fromXml(xml);
    if (!tree.hasType(presetTag))
        return juce::Result::fail("Not a Lattice preset");
    const int version = tree.getProperty(ids::formatVersion, 0);
    const int setVersion = tree.getProperty(setFormatVersion, 0);
    if (version > formatVersion || setVersion > LiveSet::currentFormatVersion)
        return juce::Result::fail("This preset was saved by a newer version of Lattice");
    if (version < 1 || setVersion < 3) // devices exist from set format 3 on
        return juce::Result::fail("Unknown preset format");
    const auto device = tree.getChildWithName(ids::device);
    if (!device.isValid() || device[ids::deviceType].toString().isEmpty())
        return juce::Result::fail("The preset holds no device");
    // Older formats are migrated here, as LiveSet does for sets.
    out.name = tree[presetName].toString();
    out.device = device.createCopy();
    return juce::Result::ok();
}

juce::Result read(const juce::File& file, Preset& out)
{
    if (!file.existsAsFile())
        return juce::Result::fail("No such preset: " + file.getFullPathName());
    auto result = parse(file.loadFileAsString(), out);
    if (result.wasOk() && out.name.isEmpty())
        out.name = file.getFileNameWithoutExtension();
    return result;
}

// --- PresetLibrary ---------------------------------------------------------------------------------

PresetLibrary::PresetLibrary(juce::File userRoot, const DeviceRegistry& r) : root(std::move(userRoot)), registry(r)
{
    rescan();
}

void PresetLibrary::rescan()
{
    presets.clear();
    scanFactory();
    scanUser();
    sort();
}

void PresetLibrary::scanFactory()
{
    for (int i = 0; i < LatticeResources::namedResourceListSize; ++i)
    {
        const auto* file = LatticeResources::originalFilenames[i];
        if (!juce::String(file).endsWithIgnoreCase(fileExtension))
            continue;
        int size = 0;
        const auto* data = LatticeResources::getNamedResource(LatticeResources::namedResourceList[i], size);
        Preset p;
        if (data == nullptr || parse(juce::String::fromUTF8(data, size), p).failed())
        {
            jassertfalse; // a broken factory preset: the tests catch this
            continue;
        }
        PresetInfo info;
        info.name = p.name.isNotEmpty() ? p.name : juce::File::createFileWithoutCheckingPath(file).getFileNameWithoutExtension();
        info.deviceType = p.device[ids::deviceType].toString();
        info.factory = true;
        info.factoryResource = LatticeResources::namedResourceList[i];
        presets.push_back(std::move(info));
    }
}

void PresetLibrary::scanUser()
{
    if (!root.isDirectory())
        return;
    for (const auto& entry : juce::RangedDirectoryIterator(root, true, fileWildcard, juce::File::findFiles))
    {
        // Only the header is needed, but presets are small: parse the whole file.
        Preset p;
        if (read(entry.getFile(), p).failed())
            continue; // not ours, or from a newer version: leave it alone
        PresetInfo info;
        info.name = p.name;
        info.deviceType = p.device[ids::deviceType].toString();
        info.file = entry.getFile();
        presets.push_back(std::move(info));
    }
}

void PresetLibrary::sort()
{
    std::stable_sort(presets.begin(), presets.end(), [](const PresetInfo& a, const PresetInfo& b) {
        if (a.factory != b.factory)
            return a.factory;
        if (a.deviceType != b.deviceType)
            return a.deviceType < b.deviceType;
        return a.name.compareNatural(b.name) < 0;
    });
}

std::vector<PresetInfo> PresetLibrary::presetsFor(const juce::String& deviceType) const
{
    std::vector<PresetInfo> out;
    for (const auto& p : presets)
        if (p.deviceType == deviceType)
            out.push_back(p);
    return out;
}

juce::Result PresetLibrary::load(const PresetInfo& info, Preset& out) const
{
    if (!info.factory)
        return read(info.file, out);
    int size = 0;
    const auto* data = LatticeResources::getNamedResource(info.factoryResource.toRawUTF8(), size);
    if (data == nullptr)
        return juce::Result::fail("No such factory preset: " + info.name);
    return parse(juce::String::fromUTF8(data, size), out);
}

juce::File PresetLibrary::userFolderFor(const juce::String& deviceType) const
{
    const auto* type = registry.find(deviceType);
    const auto folder = juce::File::createLegalFileName(type != nullptr ? type->name : deviceType);
    return root.getChildFile(folder.isNotEmpty() ? folder : "Other");
}

juce::Result PresetLibrary::saveUserPreset(const juce::ValueTree& device, const juce::String& name, juce::File& written)
{
    const auto type = device[ids::deviceType].toString();
    written = userFolderFor(type).getChildFile(fileNameFor(name));
    auto result = write(device, name.trim().isNotEmpty() ? name.trim() : "Preset", written);
    if (result.wasOk())
        rescan();
    return result;
}

} // namespace lattice::presets
