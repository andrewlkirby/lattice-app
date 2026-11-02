#pragma once

#include "core/JobPool.h"
#include "library/LibraryIndex.h"
#include "library/PresetPreviews.h"
#include "library/WaveformPeaks.h"
#include "platform/FileWatcher.h"

#include <juce_events/juce_events.h>

#include <memory>

namespace lattice
{
class DeviceRegistry;
class TuningRegistry;
namespace presets
{
class PresetLibrary;
}
} // namespace lattice

namespace lattice::library
{

/** Everything the browser shows, kept up to date (docs/05-m4-ui.md, 4e):
    - the index (LibraryIndex), with what's built in (devices, factory presets, tunings) and every
      place's files, scanned on the job pool at startup and whenever a place is added;
    - a watcher on every place, so files added, removed or renamed show up without a restart (each
      change rescans only the folder it happened in);
    - the waveform peak cache, and the preset previews.

    The User Library (`<userdata>/Library`, where presets are saved) is always a place. Message
    thread; changes are announced with a change message (after the index changed, coalesced). */
class Library final : public juce::ChangeBroadcaster, private juce::AsyncUpdater
{
public:
    /** databaseFile: empty for an in-memory index (tests). */
    Library(const juce::File& userData, const juce::File& databaseFile, const DeviceRegistry& devices,
            presets::PresetLibrary& presets, TuningRegistry& tunings, JobPool& jobs);
    ~Library() override;

    static juce::File defaultDatabaseFile(const juce::File& userData)
    {
        // Beside the User Library, not in it: that folder is watched, and the index's own writes
        // would set off rescans.
        return userData.getChildFile("Library index.sqlite");
    }

    LibraryIndex& index() noexcept { return db; }
    const PeakCache& peaks() const noexcept { return peakCache; }
    PresetPreviews& presetPreviews() noexcept { return previews; }
    const juce::File& userLibrary() const noexcept { return userFolder; }

    /** The places, the User Library first. */
    juce::Array<juce::File> places() const;
    void addPlace(const juce::File& folder);
    void removePlace(const juce::File& folder);

    /** Sets what's built in again (after presets or tunings change). */
    void refreshBuiltIns();
    /** Scans every place again. */
    void rescanAll();

    /** True while a scan is running or waiting. */
    bool scanning() const noexcept { return !scans.empty(); }

    /** Tests (no message loop): delivers the watcher's changes and the change message now. */
    void deliverForTests();

private:
    void scanLater(const juce::File& place, const juce::File& within = {});
    void handleAsyncUpdate() override;
    juce::File placeOf(const juce::File& folder) const;

    juce::File userFolder;
    const DeviceRegistry& devices;
    presets::PresetLibrary& presetLibrary;
    TuningRegistry& tunings;
    JobPool& jobs;
    LibraryIndex db;
    PeakCache peakCache;
    PresetPreviews previews;
    platform::FileWatcher watcher;
    std::vector<std::uint64_t> scans;
    std::atomic<bool> indexChanged{false};
    std::shared_ptr<int> alive = std::make_shared<int>(0);
};

} // namespace lattice::library
