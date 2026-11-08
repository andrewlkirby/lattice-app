#include "library/LibraryIndex.h"

#include "core/model/LiveSet.h"
#include "devices/framework/DeviceRegistry.h"
#include "devices/presets/Presets.h"
#include "devices/racks/Rack.h"
#include "devices/samples/SamplePool.h"
#include "tuning/parsers/ColundiTextParser.h"
#include "tuning/parsers/ScalaParser.h"
#include "tuning/parsers/TunParser.h"

#include <map>

namespace lattice::library
{

namespace
{
constexpr int batchSize = 200; // files per transaction while scanning: searches run in between

const char* const itemColumns =
    "i.id, i.kind, i.key, i.name, i.category, i.device_type, i.place, i.folder, i.size, i.modified, i.seconds, "
    "i.channels, i.sample_rate, i.root_hz, i.detail, "
    "(SELECT COALESCE(SUM(1 << c.colour), 0) FROM collections c WHERE c.key = i.key)";

Item readItem(const SqliteDb::Statement& s)
{
    Item item;
    item.id = s.integer(0);
    item.kind = static_cast<Kind>(s.integer(1));
    item.key = s.text(2);
    item.name = s.text(3);
    item.category = s.text(4);
    item.deviceType = s.text(5);
    item.place = s.text(6);
    item.folder = s.text(7);
    item.size = s.integer(8);
    item.modified = s.integer(9);
    item.seconds = s.real(10);
    item.channels = static_cast<int>(s.integer(11));
    item.sampleRate = s.real(12);
    item.rootHz = s.real(13);
    item.detail = s.text(14);
    item.collections = static_cast<std::uint32_t>(s.integer(15));
    return item;
}

bool isHidden(const juce::File& f)
{
    return f.getFileName().startsWithChar('.') || f.isHidden();
}

juce::String withSeparator(const juce::String& folder)
{
    return folder.endsWithChar(juce::File::getSeparatorChar()) ? folder : folder + juce::File::getSeparatorString();
}
} // namespace

juce::String categoryOf(const DeviceRegistry& registry, const juce::String& type, bool preset)
{
    if (type == RackDevice::drumRackId)
        return preset ? category::drums : category::instruments; // a preset of one is a kit
    if (type == RackDevice::instrumentRackId)
        return category::instruments;
    if (type == RackDevice::effectRackId)
        return category::audioEffects;
    if (const auto* info = registry.find(type))
    {
        switch (info->kind)
        {
        case DeviceKind::Instrument:
            return category::instruments;
        case DeviceKind::MidiEffect:
            return category::midiEffects;
        case DeviceKind::AudioEffect:
            return category::audioEffects;
        case DeviceKind::Rack:
            break;
        }
    }
    return category::instruments;
}

juce::String sampleDetail(double seconds, double sampleRate, int channels)
{
    const auto length = seconds < 60.0 ? juce::String(seconds, 2) + " s"
                                       : juce::String(static_cast<int>(seconds) / 60) + ":" +
                                             juce::String(static_cast<int>(seconds) % 60).paddedLeft('0', 2);
    const auto rate = juce::String(sampleRate / 1000.0, std::fmod(sampleRate, 1000.0) == 0.0 ? 0 : 1) + " kHz";
    return length + " \xc2\xb7 " + rate + " \xc2\xb7 " + (channels == 1 ? "mono" : channels == 2 ? "stereo" : juce::String(channels) + " ch");
}

juce::String LibraryIndex::collectionName(int c)
{
    static const char* const names[numCollections] = {"Favourites", "Orange", "Yellow", "Green", "Blue", "Purple", "Grey"};
    return c >= 0 && c < numCollections ? names[c] : juce::String();
}

LibraryIndex::LibraryIndex(const juce::File& databaseFile, const DeviceRegistry& registry) : devices(registry)
{
    if (databaseFile != juce::File())
        databaseFile.getParentDirectory().createDirectory();
    opened = db.open(databaseFile == juce::File() ? juce::String() : databaseFile.getFullPathName());
    if (opened.wasOk())
        opened = createSchema();
    if (opened.failed() && databaseFile != juce::File())
    {
        // A damaged index is only a cache: start it again.
        db.close();
        databaseFile.deleteFile();
        juce::File(databaseFile.getFullPathName() + "-wal").deleteFile();
        juce::File(databaseFile.getFullPathName() + "-shm").deleteFile();
        opened = db.open(databaseFile.getFullPathName());
        if (opened.wasOk())
            opened = createSchema();
    }
}

LibraryIndex::~LibraryIndex() = default;

juce::Result LibraryIndex::createSchema()
{
    // The schema's version: an index of another version is built again (it's only a cache, but
    // places and collections are kept).
    static constexpr int schemaVersion = 1;
    auto r = db.exec("PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; PRAGMA temp_store=MEMORY;");
    if (r.failed())
        return r;
    int version = 0;
    {
        SqliteDb::Statement s(db, "PRAGMA user_version");
        if (s.step())
            version = static_cast<int>(s.integer(0));
    }
    if (version != 0 && version != schemaVersion)
    {
        r = db.exec("DROP TABLE IF EXISTS items; DROP TABLE IF EXISTS items_fts;");
        if (r.failed())
            return r;
    }
    r = db.exec(R"sql(
        CREATE TABLE IF NOT EXISTS items(
            id INTEGER PRIMARY KEY,
            key TEXT NOT NULL UNIQUE,
            kind INTEGER NOT NULL,
            category TEXT NOT NULL,
            name TEXT NOT NULL,
            device_type TEXT NOT NULL DEFAULT '',
            place TEXT NOT NULL DEFAULT '',
            folder TEXT NOT NULL DEFAULT '',
            size INTEGER NOT NULL DEFAULT 0,
            modified INTEGER NOT NULL DEFAULT 0,
            seconds REAL NOT NULL DEFAULT 0,
            channels INTEGER NOT NULL DEFAULT 0,
            sample_rate REAL NOT NULL DEFAULT 0,
            root_hz REAL NOT NULL DEFAULT 0,
            detail TEXT NOT NULL DEFAULT '');
        CREATE INDEX IF NOT EXISTS items_by_place ON items(place);
        CREATE INDEX IF NOT EXISTS items_by_folder ON items(folder, name);
        CREATE INDEX IF NOT EXISTS items_by_category ON items(category, name);
        CREATE INDEX IF NOT EXISTS items_by_device ON items(device_type, kind);
        CREATE VIRTUAL TABLE IF NOT EXISTS items_fts USING fts5(
            name, tags, category, words, tokenize = 'unicode61 remove_diacritics 2', prefix = '1 2 3');
        CREATE TABLE IF NOT EXISTS places(path TEXT PRIMARY KEY);
        CREATE TABLE IF NOT EXISTS collections(key TEXT NOT NULL, colour INTEGER NOT NULL, PRIMARY KEY(key, colour));
        CREATE TABLE IF NOT EXISTS plugin_files(
            format TEXT NOT NULL,
            identifier TEXT NOT NULL,
            size INTEGER NOT NULL DEFAULT 0,
            modified INTEGER NOT NULL DEFAULT 0,
            status INTEGER NOT NULL,
            reason TEXT NOT NULL DEFAULT '',
            scanned INTEGER NOT NULL DEFAULT 0,
            PRIMARY KEY(format, identifier));
        CREATE TABLE IF NOT EXISTS plugins(
            key TEXT PRIMARY KEY,
            format TEXT NOT NULL,
            identifier TEXT NOT NULL,
            xml TEXT NOT NULL);
        CREATE INDEX IF NOT EXISTS plugins_by_file ON plugins(format, identifier);
    )sql");
    if (r.failed())
        return r;
    return db.exec(("PRAGMA user_version=" + juce::String(schemaVersion)).toRawUTF8());
}

void LibraryIndex::changed()
{
    ++changes;
    if (onChanged)
        onChanged();
}

juce::String LibraryIndex::matchExpression(const juce::String& typed)
{
    // The words FTS5's unicode61 tokenizer makes: runs of letters and digits.
    juce::StringArray words;
    juce::String word;
    const auto flush = [&] {
        if (word.isNotEmpty())
            words.add("\"" + word.toLowerCase() + "\"*");
        word.clear();
    };
    for (auto p = typed.getCharPointer(); !p.isEmpty(); ++p)
    {
        const auto c = *p;
        if (juce::CharacterFunctions::isLetterOrDigit(c) || c > 127)
            word += c;
        else
            flush();
    }
    flush();
    return words.joinIntoString(" ");
}

juce::String LibraryIndex::searchWords(const Item& item) const
{
    // Everything else a search may match: the path's words, the details, the extension, the device.
    juce::String words = item.detail;
    if (item.place.isNotEmpty())
        words << " " << item.key.substring(item.place.length()) << " " << juce::File(item.key).getFileExtension().substring(1);
    if (item.deviceType.isNotEmpty())
        if (const auto* info = devices.find(item.deviceType))
            words << " " << info->name;
    if (item.kind == Kind::preset)
        words << " preset";
    if (item.kind == Kind::tuning)
        words << " tuning";
    if (item.kind == Kind::plugin)
        words << " plugin";
    if (item.rootHz > 0.0)
        words << " root " << juce::String(item.rootHz, 2) << " Hz";
    return words;
}

juce::String LibraryIndex::tagsFor(const juce::String& key) const
{
    juce::StringArray names;
    SqliteDb::Statement s(db, "SELECT colour FROM collections WHERE key = ?1 ORDER BY colour");
    s.bind(1, key);
    while (s.step())
        names.add(collectionName(static_cast<int>(s.integer(0))));
    return names.joinIntoString(" ");
}

void LibraryIndex::writeItem(const Item& item)
{
    std::int64_t id = 0;
    {
        SqliteDb::Statement find(db, "SELECT id FROM items WHERE key = ?1");
        find.bind(1, item.key);
        if (find.step())
            id = find.integer(0);
    }
    SqliteDb::Statement write(db, id != 0 ? "UPDATE items SET kind=?2, category=?3, name=?4, device_type=?5, place=?6, folder=?7, "
                                            "size=?8, modified=?9, seconds=?10, channels=?11, sample_rate=?12, root_hz=?13, "
                                            "detail=?14 WHERE key=?1"
                                          : "INSERT INTO items(key, kind, category, name, device_type, place, folder, size, "
                                            "modified, seconds, channels, sample_rate, root_hz, detail) "
                                            "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12, ?13, ?14)");
    write.bind(1, item.key)
        .bind(2, static_cast<int>(item.kind))
        .bind(3, item.category)
        .bind(4, item.name)
        .bind(5, item.deviceType)
        .bind(6, item.place)
        .bind(7, item.folder)
        .bind(8, item.size)
        .bind(9, item.modified)
        .bind(10, item.seconds)
        .bind(11, item.channels)
        .bind(12, item.sampleRate)
        .bind(13, item.rootHz)
        .bind(14, item.detail);
    write.run();
    if (id == 0)
        id = db.lastInsertId();
    SqliteDb::Statement drop(db, "DELETE FROM items_fts WHERE rowid = ?1");
    drop.bind(1, id).run();
    SqliteDb::Statement text(db, "INSERT INTO items_fts(rowid, name, tags, category, words) VALUES(?1, ?2, ?3, ?4, ?5)");
    text.bind(1, id).bind(2, item.name).bind(3, tagsFor(item.key)).bind(4, item.category).bind(5, searchWords(item)).run();
}

void LibraryIndex::removeItem(std::int64_t id)
{
    SqliteDb::Statement a(db, "DELETE FROM items WHERE id = ?1");
    a.bind(1, id).run();
    SqliteDb::Statement b(db, "DELETE FROM items_fts WHERE rowid = ?1");
    b.bind(1, id).run();
}

void LibraryIndex::setBuiltIns(const std::vector<Item>& items)
{
    {
        const std::lock_guard<std::mutex> hold(lock);
        SqliteDb::Transaction t(db);
        std::vector<std::int64_t> old;
        {
            // The plugins aren't built in: the plugin scan sets them (setPluginItems).
            SqliteDb::Statement s(db, ("SELECT id FROM items WHERE place = '' AND category <> '" + juce::String(category::plugins) +
                                       "'").toRawUTF8());
            while (s.step())
                old.push_back(s.integer(0));
        }
        for (const auto id : old)
            removeItem(id);
        for (auto item : items)
        {
            item.place.clear();
            writeItem(item);
        }
        t.commit();
    }
    changed();
}

juce::StringArray LibraryIndex::places() const
{
    const std::lock_guard<std::mutex> hold(lock);
    juce::StringArray result;
    SqliteDb::Statement s(db, "SELECT path FROM places ORDER BY path COLLATE NOCASE");
    while (s.step())
        result.add(s.text(0));
    return result;
}

void LibraryIndex::addPlace(const juce::File& folder)
{
    {
        const std::lock_guard<std::mutex> hold(lock);
        SqliteDb::Statement s(db, "INSERT OR IGNORE INTO places(path) VALUES(?1)");
        s.bind(1, folder.getFullPathName()).run();
    }
    changed();
}

void LibraryIndex::removePlace(const juce::File& folder)
{
    {
        const std::lock_guard<std::mutex> hold(lock);
        SqliteDb::Transaction t(db);
        SqliteDb::Statement s(db, "DELETE FROM places WHERE path = ?1");
        s.bind(1, folder.getFullPathName()).run();
        std::vector<std::int64_t> ids;
        {
            SqliteDb::Statement q(db, "SELECT id FROM items WHERE place = ?1");
            q.bind(1, folder.getFullPathName());
            while (q.step())
                ids.push_back(q.integer(0));
        }
        for (const auto id : ids)
            removeItem(id);
        t.commit();
    }
    changed();
}

bool LibraryIndex::describe(const juce::File& file, const juce::File& place, Item& out) const
{
    out = {};
    out.key = file.getFullPathName();
    out.place = place.getFullPathName();
    out.folder = file.getParentDirectory().getFullPathName();
    out.name = file.getFileNameWithoutExtension();
    if (file.isDirectory())
    {
        out.kind = Kind::folder;
        out.category = category::folders;
        out.name = file.getFileName();
        return true;
    }
    out.size = file.getSize();
    out.modified = file.getLastModificationTime().toMilliseconds();
    if (file.hasFileExtension("wav;aif;aiff;flac;ogg;mp3"))
    {
        std::unique_ptr<juce::AudioFormatReader> reader(SamplePool::formats().createReaderFor(file));
        if (reader == nullptr || reader->sampleRate <= 0.0)
            return false;
        out.kind = Kind::sample;
        out.category = category::samples;
        out.sampleRate = reader->sampleRate;
        out.channels = static_cast<int>(reader->numChannels);
        out.seconds = static_cast<double>(reader->lengthInSamples) / reader->sampleRate;
        const auto unity = reader->metadataValues.getValue("MidiUnityNote", {});
        if (unity.isNotEmpty())
            out.rootHz = 440.0 * std::exp2((unity.getIntValue() - 69) / 12.0);
        out.detail = sampleDetail(out.seconds, out.sampleRate, out.channels);
        return true;
    }
    if (file.hasFileExtension(presets::fileExtension))
    {
        presets::Preset preset;
        if (presets::read(file, preset).failed())
            return false;
        out.kind = Kind::preset;
        out.deviceType = preset.device[ids::deviceType].toString();
        out.category = categoryOf(devices, out.deviceType, true);
        if (preset.name.isNotEmpty())
            out.name = preset.name;
        const auto* info = devices.find(out.deviceType);
        out.detail = (info != nullptr ? info->name : out.deviceType) + " preset";
        return true;
    }
    if (file.hasFileExtension("scl"))
    {
        const auto scale = ScalaParser::parseScale(file.loadFileAsString());
        if (!scale.scale)
            return false;
        out.kind = Kind::tuning;
        out.category = category::tunings;
        out.detail = juce::String(static_cast<int>(scale.scale->ratios.size())) + " notes";
        if (scale.scale->description.isNotEmpty())
            out.detail << " \xc2\xb7 " << scale.scale->description;
        return true;
    }
    if (file.hasFileExtension("tun"))
    {
        const auto tuning = TunParser::parse(file.loadFileAsString(), {}, file.getFileNameWithoutExtension(), {});
        if (!tuning.ok())
            return false;
        out.kind = Kind::tuning;
        out.category = category::tunings;
        out.detail = "128 notes \xc2\xb7 AnaMark .tun";
        return true;
    }
    if (file.hasFileExtension("txt") && out.size < 256 * 1024)
    {
        // Only a Colundi-format table (128 rows of frequencies) is a tuning.
        ColundiTextParser::Options options;
        options.id = "probe";
        if (!ColundiTextParser::parse(file.loadFileAsString(), options).ok())
            return false;
        out.kind = Kind::tuning;
        out.category = category::tunings;
        out.detail = "128-entry table";
        return true;
    }
    return false;
}

LibraryIndex::ScanStats LibraryIndex::scan(const juce::File& place, const std::function<bool()>& cancelled,
                                           const std::function<void(float)>& progress, const juce::File& within)
{
    ScanStats stats;
    const auto placePath = place.getFullPathName();
    const auto scope = within == juce::File() ? place : within;
    const auto scopePath = scope.getFullPathName();
    const auto prefix = withSeparator(scopePath);

    // What the index has now, in this scope.
    struct Known
    {
        std::int64_t id = 0, size = 0, modified = 0;
        bool seen = false;
    };
    std::map<juce::String, Known> known;
    {
        const std::lock_guard<std::mutex> hold(lock);
        SqliteDb::Statement s(db, "SELECT id, key, size, modified FROM items WHERE place = ?1 AND "
                                  "(key = ?2 OR substr(key, 1, ?3) = ?4)");
        s.bind(1, placePath).bind(2, scopePath).bind(3, prefix.length()).bind(4, prefix);
        while (s.step())
            known[s.text(1)] = {s.integer(0), s.integer(2), s.integer(3), false};
    }

    std::vector<Item> pending;
    int batches = 0;
    const auto writePending = [&] {
        if (pending.empty())
            return;
        const std::lock_guard<std::mutex> hold(lock);
        SqliteDb::Transaction t(db);
        for (const auto& item : pending)
            writeItem(item);
        t.commit();
        pending.clear();
        ++changes;
        // A long first scan shows what it has found every few thousand files.
        if (++batches % 10 == 0 && onChanged)
            onChanged();
    };

    const auto visit = [&](const juce::File& f) {
        const auto key = f.getFullPathName();
        auto found = known.find(key);
        if (found != known.end())
        {
            found->second.seen = true;
            if (f.isDirectory() || (found->second.size == f.getSize() &&
                                    found->second.modified == f.getLastModificationTime().toMilliseconds()))
            {
                ++stats.unchanged;
                return;
            }
        }
        Item item;
        if (!describe(f, place, item))
            return;
        if (found != known.end())
            ++stats.changed;
        else
            ++stats.added;
        pending.push_back(std::move(item));
        if (static_cast<int>(pending.size()) >= batchSize)
            writePending();
    };

    if (scope != place && scope.isDirectory())
        visit(scope); // the folder itself, when a watcher names it
    if (scope.isDirectory())
    {
        int visited = 0;
        for (const auto& entry : juce::RangedDirectoryIterator(scope, true, "*", juce::File::findFilesAndDirectories,
                                                                juce::File::FollowSymlinks::noCycles))
        {
            if (cancelled && (++visited & 63) == 0 && cancelled())
            {
                stats.cancelled = true;
                break;
            }
            const auto& f = entry.getFile();
            // Nothing inside hidden folders either.
            bool hidden = false;
            for (auto p = f; p != scope && p != p.getParentDirectory(); p = p.getParentDirectory())
                if (isHidden(p))
                {
                    hidden = true;
                    break;
                }
            if (hidden)
                continue;
            visit(f);
            if (progress && (visited & 255) == 0)
                progress(entry.getEstimatedProgress());
        }
    }
    writePending();

    if (!stats.cancelled)
    {
        // What's gone from the disk (or isn't something the library shows any more).
        const std::lock_guard<std::mutex> hold(lock);
        SqliteDb::Transaction t(db);
        for (const auto& [key, k] : known)
            if (!k.seen)
            {
                removeItem(k.id);
                ++stats.removed;
            }
        t.commit();
    }
    if (stats.total() > 0)
        changed();
    return stats;
}

std::vector<Item> LibraryIndex::find(const Query& query) const
{
    const auto match = matchExpression(query.text);
    juce::String sql = "SELECT " + juce::String(itemColumns);
    if (match.isNotEmpty())
        sql << ", CASE WHEN i.name = ?1 COLLATE NOCASE THEN 0 WHEN i.name LIKE ?2 ESCAPE '\\' THEN 1 ELSE 2 END AS m"
               " FROM items_fts JOIN items i ON i.id = items_fts.rowid WHERE items_fts MATCH ?3";
    else
        sql << " FROM items i WHERE 1";
    if (query.category.isNotEmpty())
        sql << " AND (i.category = ?4 OR (?4 = '" << category::drums << "' AND i.kind = " << static_cast<int>(Kind::device)
            << " AND i.device_type = '" << RackDevice::drumRackId << "'))";
    if (query.folder.isNotEmpty())
        sql << " AND i.folder = ?5";
    if (query.collection >= 0)
        sql << " AND EXISTS (SELECT 1 FROM collections c WHERE c.key = i.key AND c.colour = ?6)";
    if (query.kind >= 0)
        sql << " AND i.kind = ?7";
    if (query.deviceType.isNotEmpty())
        sql << " AND i.device_type = ?8";
    if (match.isNotEmpty())
        sql << " ORDER BY m, bm25(items_fts, 10.0, 4.0, 2.0, 1.0), length(i.name), i.name COLLATE NOCASE";
    else if (query.folder.isNotEmpty())
        sql << " ORDER BY i.kind <> " << static_cast<int>(Kind::folder) << ", i.name COLLATE NOCASE";
    else
        sql << " ORDER BY i.kind, i.name COLLATE NOCASE";
    if (query.limit > 0)
        sql << " LIMIT " << query.limit;

    const std::lock_guard<std::mutex> hold(lock);
    SqliteDb::Statement s(db, sql.toRawUTF8());
    if (match.isNotEmpty())
    {
        const auto typed = query.text.trim();
        s.bind(1, typed);
        s.bind(2, typed.replace("\\", "\\\\").replace("%", "\\%").replace("_", "\\_") + "%");
        s.bind(3, match);
    }
    if (query.category.isNotEmpty())
        s.bind(4, query.category);
    if (query.folder.isNotEmpty())
        s.bind(5, query.folder);
    if (query.collection >= 0)
        s.bind(6, query.collection);
    if (query.kind >= 0)
        s.bind(7, query.kind);
    if (query.deviceType.isNotEmpty())
        s.bind(8, query.deviceType);
    std::vector<Item> result;
    while (s.step())
        result.push_back(readItem(s));
    return result;
}

std::int64_t LibraryIndex::count() const
{
    const std::lock_guard<std::mutex> hold(lock);
    SqliteDb::Statement s(db, "SELECT COUNT(*) FROM items");
    return s.step() ? s.integer(0) : 0;
}

std::vector<Item> LibraryIndex::itemsWithKey(const juce::String& key) const
{
    const std::lock_guard<std::mutex> hold(lock);
    SqliteDb::Statement s(db, ("SELECT " + juce::String(itemColumns) + " FROM items i WHERE i.key = ?1").toRawUTF8());
    s.bind(1, key);
    std::vector<Item> result;
    while (s.step())
        result.push_back(readItem(s));
    return result;
}

void LibraryIndex::setInCollection(const juce::String& key, int collection, bool in)
{
    if (collection < 0 || collection >= numCollections)
        return;
    {
        const std::lock_guard<std::mutex> hold(lock);
        SqliteDb::Transaction t(db);
        SqliteDb::Statement s(db, in ? "INSERT OR IGNORE INTO collections(key, colour) VALUES(?1, ?2)"
                                     : "DELETE FROM collections WHERE key = ?1 AND colour = ?2");
        s.bind(1, key).bind(2, collection).run();
        // The collections' names are searchable ("favourites").
        SqliteDb::Statement ids(db, "SELECT id FROM items WHERE key = ?1");
        ids.bind(1, key);
        const auto tags = tagsFor(key);
        while (ids.step())
        {
            SqliteDb::Statement u(db, "UPDATE items_fts SET tags = ?2 WHERE rowid = ?1");
            u.bind(1, ids.integer(0)).bind(2, tags).run();
        }
        t.commit();
    }
    changed();
}

// --- Plugins -----------------------------------------------------------------------------------------

std::vector<PluginFile> LibraryIndex::pluginFiles() const
{
    const std::lock_guard<std::mutex> hold(lock);
    std::vector<PluginFile> result;
    SqliteDb::Statement s(db, "SELECT format, identifier, size, modified, status, reason, scanned FROM plugin_files "
                              "ORDER BY format, identifier");
    while (s.step())
    {
        PluginFile f;
        f.format = s.text(0);
        f.identifier = s.text(1);
        f.size = s.integer(2);
        f.modified = s.integer(3);
        f.status = static_cast<PluginFile::Status>(juce::jlimit(0, 4, static_cast<int>(s.integer(4))));
        f.reason = s.text(5);
        f.scanned = s.integer(6);
        result.push_back(std::move(f));
    }
    return result;
}

void LibraryIndex::recordPluginFile(const PluginFile& file, const std::vector<StoredPlugin>& plugins)
{
    {
        const std::lock_guard<std::mutex> hold(lock);
        SqliteDb::Transaction t(db);
        SqliteDb::Statement w(db, "INSERT OR REPLACE INTO plugin_files(format, identifier, size, modified, status, reason, scanned) "
                                  "VALUES(?1, ?2, ?3, ?4, ?5, ?6, ?7)");
        w.bind(1, file.format)
            .bind(2, file.identifier)
            .bind(3, file.size)
            .bind(4, file.modified)
            .bind(5, static_cast<int>(file.status))
            .bind(6, file.reason)
            .bind(7, file.scanned)
            .run();
        SqliteDb::Statement drop(db, "DELETE FROM plugins WHERE format = ?1 AND identifier = ?2");
        drop.bind(1, file.format).bind(2, file.identifier).run();
        for (const auto& p : plugins)
        {
            SqliteDb::Statement add(db, "INSERT OR REPLACE INTO plugins(key, format, identifier, xml) VALUES(?1, ?2, ?3, ?4)");
            add.bind(1, p.key).bind(2, file.format).bind(3, file.identifier).bind(4, p.xml).run();
        }
        t.commit();
    }
    ++changes;
}

void LibraryIndex::removePluginFiles(const std::vector<std::pair<juce::String, juce::String>>& formatAndIdentifier)
{
    if (formatAndIdentifier.empty())
        return;
    {
        const std::lock_guard<std::mutex> hold(lock);
        SqliteDb::Transaction t(db);
        for (const auto& [format, identifier] : formatAndIdentifier)
        {
            SqliteDb::Statement a(db, "DELETE FROM plugin_files WHERE format = ?1 AND identifier = ?2");
            a.bind(1, format).bind(2, identifier).run();
            SqliteDb::Statement b(db, "DELETE FROM plugins WHERE format = ?1 AND identifier = ?2");
            b.bind(1, format).bind(2, identifier).run();
        }
        t.commit();
    }
    ++changes;
}

std::vector<StoredPlugin> LibraryIndex::storedPlugins() const
{
    const std::lock_guard<std::mutex> hold(lock);
    std::vector<StoredPlugin> result;
    SqliteDb::Statement s(db, "SELECT key, format, identifier, xml FROM plugins ORDER BY key");
    while (s.step())
        result.push_back({s.text(0), s.text(1), s.text(2), s.text(3)});
    return result;
}

void LibraryIndex::setPluginItems(const std::vector<Item>& items)
{
    {
        const std::lock_guard<std::mutex> hold(lock);
        SqliteDb::Transaction t(db);
        std::vector<std::int64_t> old;
        {
            SqliteDb::Statement s(db, "SELECT id FROM items WHERE category = ?1");
            s.bind(1, juce::String(category::plugins));
            while (s.step())
                old.push_back(s.integer(0));
        }
        for (const auto id : old)
            removeItem(id);
        for (auto item : items)
        {
            item.category = category::plugins;
            item.place.clear();
            writeItem(item);
        }
        t.commit();
    }
    changed();
}

} // namespace lattice::library
