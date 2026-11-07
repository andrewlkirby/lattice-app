#pragma once

#include "hosting/scan/PluginScanner.h"
#include "library/LibraryIndex.h"

#include <juce_events/juce_events.h>

#include <atomic>
#include <mutex>
#include <set>

namespace lattice::hosting
{

/** How the browser groups the plugins. */
enum class PluginGrouping
{
    vendor = 0,
    format = 1,
    type = 2, // Instruments, Audio Effects, MIDI Effects
};

struct PluginSettings
{
    bool useDefaultFolders = true;  // each format's standard folders
    juce::StringArray extraFolders; // searched for every format that has files
    juce::String preferredFormat;   // shown when a plugin comes in several (empty: CLAP, then VST3...)
    PluginGrouping grouping = PluginGrouping::vendor;
    double timeLimitSeconds = 30.0; // for each file
};

/** The plugins installed (docs/07-m6-plugins-external.md, 6a): found in the formats' folders,
    described by LatticePluginScanner in the background, and kept in the library index, from which
    the browser's Plugins category is made.

    - Scans are incremental: a file is scanned again only when its size or modification time
      changed (or it's asked for). Files gone from the disk are forgotten.
    - A file the scanner crashes on or hangs on is blocked: kept with the reason, and not scanned
      again until it changes or a retry is asked for.
    - When a plugin comes in several formats, the browser shows it once, in the preferred format.

    Message thread, except the scan itself (a thread of its own, at low priority). Changes (the
    status, the plugins found) are announced with a change message. */
class PluginLibrary final : public juce::ChangeBroadcaster, private juce::Thread, private juce::AsyncUpdater
{
public:
    PluginLibrary(library::LibraryIndex& index, juce::File scannerExecutable, juce::File workFolder);
    ~PluginLibrary() override;

    /** Where LatticePluginScanner is beside the running app. */
    static juce::File scannerBesideApp();

    const PluginSettings& settings() const noexcept { return current; }
    /** Folders changed: scans again (unless `scanIfFoldersChanged` is false: the settings the app
        starts with, before its scan). Grouping or format changed: the browser's rows are made again. */
    void setSettings(const PluginSettings& settings, bool scanIfFoldersChanged = true);

    /** Starts a scan in the background (incremental, or every file not blocked). After `delayMs`. */
    void scan(bool everything = false, int delayMs = 0);
    /** Scans these files again, blocked or not. */
    void retry(const std::vector<library::PluginFile>& files);
    void cancel();

    struct Status
    {
        bool scanning = false;
        int done = 0, total = 0; // files, in this scan
        int plugins = 0;         // found, counting a plugin in several formats once
        juce::String formats;    // how many in each format: "CLAP 2, VST3 2"
        juce::String lastError;  // why the last scan stopped early, if it did
    };
    Status status() const;

    /** Files that are blocked or that failed, with why. */
    std::vector<library::PluginFile> problems() const;

    /** Tests (no message loop): waits for the scan to finish, then delivers its change message. */
    bool waitForTests(int timeoutMs);

    /** Folders searched for a format, from these settings. */
    static juce::FileSearchPath foldersFor(juce::AudioPluginFormat& format, const PluginSettings& settings);
    /** What a file's change is seen by: its size and modification time (for a bundle, its files'). */
    static std::pair<juce::int64, juce::int64> stampOf(const juce::String& identifier);

private:
    void run() override;
    void handleAsyncUpdate() override;
    void scanPass(bool everything, const std::set<std::pair<juce::String, juce::String>>& retries);
    void rebuildItems();

    library::LibraryIndex& index;
    juce::File scannerFile, workFolder;
    PluginSettings current;
    mutable std::mutex settingsLock; // `current`, as the scan thread reads it
    PluginSettings settingsCopy() const;

    std::mutex requestLock;
    bool requested = false, requestEverything = false, rebuildRequested = false;
    int requestDelayMs = 0;
    std::set<std::pair<juce::String, juce::String>> retries;
    std::atomic<bool> cancelRequested{false};
    std::atomic<bool> busy{false};

    mutable std::mutex statusLock;
    Status state;
};

} // namespace lattice::hosting
