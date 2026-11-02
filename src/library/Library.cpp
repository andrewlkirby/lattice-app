#include "library/Library.h"

#include "devices/framework/DeviceRegistry.h"
#include "devices/presets/Presets.h"
#include "devices/racks/Rack.h"
#include "tuning/TuningRegistry.h"

#include <algorithm>

namespace lattice::library
{

namespace
{
constexpr std::int64_t peakCacheBytes = 256LL * 1024 * 1024;

juce::String kindDetail(DeviceKind kind)
{
    switch (kind)
    {
    case DeviceKind::Instrument:
        return "Instrument";
    case DeviceKind::MidiEffect:
        return "MIDI effect";
    case DeviceKind::Rack:
        return "Rack";
    case DeviceKind::AudioEffect:
        break;
    }
    return "Audio effect";
}
} // namespace

Library::Library(const juce::File& userData, const juce::File& databaseFile, const DeviceRegistry& d,
                 presets::PresetLibrary& p, TuningRegistry& t, JobPool& j)
    : userFolder(userData.getChildFile("Library")),
      devices(d),
      presetLibrary(p),
      tunings(t),
      jobs(j),
      db(databaseFile, d),
      peakCache(userData.getChildFile("Cache").getChildFile("Peaks")),
      previews(t, d, j)
{
    userFolder.createDirectory();
    db.onChanged = [this] {
        indexChanged = true;
        triggerAsyncUpdate();
    };
    watcher.onChange = [this] { triggerAsyncUpdate(); };
    refreshBuiltIns();
    for (const auto& place : places())
    {
        watcher.watch(place);
        scanLater(place);
    }
    // The cache can't grow without end; trimming it is disk work, so it's a job too.
    jobs.submit("Trimming the waveform cache", [cache = peakCache](JobPool::Context&) { cache.prune(peakCacheBytes); });
}

Library::~Library()
{
    cancelPendingUpdate();
    db.onChanged = nullptr;
    for (const auto id : scans)
        jobs.cancel(id);
    previews.cancel();
    jobs.waitUntilIdle(5000); // scans write to the index, which goes with this
}

juce::Array<juce::File> Library::places() const
{
    juce::Array<juce::File> result{userFolder};
    for (const auto& path : db.places())
        if (juce::File::isAbsolutePath(path) && juce::File(path) != userFolder)
            result.add(juce::File(path));
    return result;
}

void Library::addPlace(const juce::File& folder)
{
    if (!folder.isDirectory() || places().contains(folder))
        return;
    db.addPlace(folder);
    watcher.watch(folder);
    scanLater(folder);
}

void Library::removePlace(const juce::File& folder)
{
    if (folder == userFolder)
        return;
    watcher.unwatch(folder);
    db.removePlace(folder);
}

juce::File Library::placeOf(const juce::File& folder) const
{
    for (const auto& place : places())
        if (folder == place || folder.isAChildOf(place))
            return place;
    return {};
}

void Library::refreshBuiltIns()
{
    std::vector<Item> items;
    for (const auto& type : devices.types())
    {
        if (type.hidden)
            continue;
        Item item;
        item.kind = Kind::device;
        item.key = "device:" + type.id;
        item.name = type.name;
        item.category = categoryOf(devices, type.id, false);
        item.deviceType = type.id;
        item.detail = kindDetail(type.kind);
        items.push_back(item);
    }
    for (const auto& preset : presetLibrary.all())
    {
        if (!preset.factory)
            continue; // user presets are files in the User Library
        Item item;
        item.kind = Kind::preset;
        item.key = presets::PresetLibrary::keyOf(preset);
        item.name = preset.name;
        item.deviceType = preset.deviceType;
        const auto* info = devices.find(preset.deviceType);
        item.category = categoryOf(devices, preset.deviceType, true);
        item.detail = (info != nullptr ? info->name : preset.deviceType) + " preset";
        items.push_back(item);
    }
    for (const auto& tuning : tunings.getAll())
    {
        if (!tuning->id.startsWith("builtin."))
            continue; // tuning files are found in places
        Item item;
        item.kind = Kind::tuning;
        item.key = "tuning:" + tuning->id;
        item.name = tuning->name;
        item.category = category::tunings;
        item.detail = tuning->usesNoteNames ? "Note names" : "128-entry table";
        items.push_back(item);
    }
    db.setBuiltIns(items);
}

void Library::rescanAll()
{
    for (const auto& place : places())
        scanLater(place);
}

void Library::scanLater(const juce::File& place, const juce::File& within)
{
    std::weak_ptr<int> life = alive;
    auto* index = &db;
    const auto name = "Scanning " + (within != juce::File() ? within : place).getFileName();
    auto id = std::make_shared<std::uint64_t>(0);
    *id = jobs.submit(
        name,
        [index, place, within](JobPool::Context& work) {
            index->scan(place, [&work] { return work.isCancelled(); }, [&work](float p) { work.setProgress(p); }, within);
        },
        [this, life, id](bool) {
            if (life.expired())
                return;
            scans.erase(std::remove(scans.begin(), scans.end(), *id), scans.end());
        });
    scans.push_back(*id);
}

void Library::handleAsyncUpdate()
{
    // What the watcher saw: rescan just those folders, each in the place it belongs to.
    for (const auto& folder : watcher.takeChanges())
        if (const auto place = placeOf(folder); place != juce::File())
            scanLater(place, folder == place ? juce::File() : folder);
    if (indexChanged.exchange(false))
        sendChangeMessage();
}

void Library::deliverForTests()
{
    handleUpdateNowIfNeeded();
    handleAsyncUpdate();
    dispatchPendingMessages();
}

} // namespace lattice::library
