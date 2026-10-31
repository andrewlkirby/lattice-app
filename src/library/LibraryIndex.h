#pragma once

#include "library/Sqlite.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

namespace lattice
{
class DeviceRegistry;
}

namespace lattice::library
{

/** What an item in the library is. */
enum class Kind
{
    device = 0, // a built-in device type
    preset = 1, // a factory preset (built in) or a .lpreset file
    sample = 2, // an audio file
    tuning = 3, // a built-in tuning, or a .scl or Colundi-format .txt file
    folder = 4, // a folder inside a place
};

/** The browser's categories (docs/05-m4-ui.md, 4e). Every item has one. */
namespace category
{
inline constexpr const char* instruments = "Instruments";
inline constexpr const char* drums = "Drums";
inline constexpr const char* audioEffects = "Audio Effects";
inline constexpr const char* midiEffects = "MIDI Effects";
inline constexpr const char* samples = "Samples";
inline constexpr const char* tunings = "Tunings";
inline constexpr const char* folders = "Folders";
} // namespace category

/** One row of the index. */
struct Item
{
    std::int64_t id = 0;
    Kind kind = Kind::sample;
    /** Stable, and what collections refer to: a file's full path; "device:<type>",
        "factory:<resource>" or "tuning:<id>" for what's built in. */
    juce::String key;
    juce::String name;
    juce::String category;
    juce::String deviceType; // devices and presets
    juce::String place;      // the place a file was found in (empty: built in)
    juce::String folder;     // the folder a file is in
    std::int64_t size = 0;
    std::int64_t modified = 0; // ms since 1970
    double seconds = 0.0;      // samples
    int channels = 0;
    double sampleRate = 0.0;
    double rootHz = 0.0; // a sample's root note from its file (0: none)
    juce::String detail; // shown beside the name: "2.50 s · 48 kHz · stereo", "Synth preset"
    std::uint32_t collections = 0; // bit c: in collection c

    juce::File file() const { return place.isNotEmpty() ? juce::File(key) : juce::File(); }
    bool inCollection(int c) const noexcept { return (collections >> c) & 1u; }
};

/** What to find. Every field narrows it; an empty query finds everything. */
struct Query
{
    juce::String text = {};     // words, each matched as the start of a word in any field
    juce::String category = {}; // one category, or all
    juce::String folder = {};   // only what's directly in this folder (browsing a place)
    int collection = -1;   // only what's in this collection
    int kind = -1;         // only items of this Kind
    juce::String deviceType = {}; // only devices and presets of this type
    int limit = 0;         // 0: no limit
};

/** The library index (docs/05-m4-ui.md, 4e): an SQLite database of everything the browser shows,
    built in the background.

    - Built-in content (devices, factory presets, built-in tunings) is set by the app at startup.
    - Files come from places (folders): their metadata (size, date, and for samples the length,
      channels, sample rate and root note), so searches never touch the disk.
    - Collections (colour tags, the first being Favourites) and places are kept by key, so they
      survive rescans and moving between sessions.
    - Search is full text (FTS5) on every field, each word matched as a word start, ranked: exact
      names, then names that start with the text, then by relevance (the name weighs most).

    Thread-safe: one connection, guarded; a scan commits in batches, so searches run in between. */
class LibraryIndex
{
public:
    static constexpr int numCollections = 7;
    static juce::String collectionName(int c);

    /** A database file (created if missing), or an in-memory one for an empty File. */
    LibraryIndex(const juce::File& databaseFile, const DeviceRegistry& registry);
    ~LibraryIndex();

    juce::Result openResult() const { return opened; }

    /** Replaces everything built in. */
    void setBuiltIns(const std::vector<Item>& items);

    /** Places: folders whose files are indexed. Adding one doesn't scan it (scan() does). */
    juce::StringArray places() const;
    void addPlace(const juce::File& folder);
    void removePlace(const juce::File& folder); // and its items

    /** Brings a place's items up to date with the disk: new and changed files are read, missing
        ones removed. With `within`, only that folder (and below) is looked at: what a watcher saw
        change. Any thread but the message thread. Returns the number of items added, changed or
        removed. */
    struct ScanStats
    {
        int added = 0, changed = 0, removed = 0, unchanged = 0;
        bool cancelled = false;
        int total() const noexcept { return added + changed + removed; }
    };
    ScanStats scan(const juce::File& place, const std::function<bool()>& cancelled = {},
                   const std::function<void(float)>& progress = {}, const juce::File& within = {});

    /** Any thread. */
    std::vector<Item> find(const Query& query) const;
    std::int64_t count() const;
    std::vector<Item> itemsWithKey(const juce::String& key) const;

    /** Collections. */
    void setInCollection(const juce::String& key, int collection, bool in);

    /** Increases on every change (scans, collections, places, built-ins). Any thread. */
    std::uint64_t generation() const noexcept { return changes.load(); }
    /** Called after a change, on the thread that made it. */
    std::function<void()> onChanged;

    /** Reads one file's item (no database): what a scan stores. False for files the library
        doesn't show. */
    bool describe(const juce::File& file, const juce::File& place, Item& out) const;

    /** The text an FTS5 MATCH gets for what was typed: each word a quoted prefix. Empty if
        nothing searchable was typed. */
    static juce::String matchExpression(const juce::String& typed);

private:
    juce::Result createSchema();
    void changed();
    void writeItem(const Item& item); // lock held, inside a transaction
    void removeItem(std::int64_t id); // lock held
    juce::String searchWords(const Item& item) const;
    juce::String tagsFor(const juce::String& key) const; // lock held

    const DeviceRegistry& devices;
    mutable std::mutex lock;
    mutable SqliteDb db;
    juce::Result opened = juce::Result::ok();
    std::atomic<std::uint64_t> changes{1};
};

/** The browser category of a device type, or of a preset for it: racks go with what they hold
    (Instrument and Drum Racks with the instruments, Audio Effect Racks with the effects), and a
    Drum Rack preset is a kit, under Drums. */
juce::String categoryOf(const DeviceRegistry& registry, const juce::String& deviceType, bool preset);

/** "2.50 s · 48 kHz · stereo". */
juce::String sampleDetail(double seconds, double sampleRate, int channels);

} // namespace lattice::library
