#include "hosting/PluginLibrary.h"

#include "hosting/PluginFormats.h"

#include <map>

namespace lattice::hosting
{

using library::Item;
using library::PluginFile;

namespace
{
juce::String typeOf(const juce::PluginDescription& d)
{
    if (d.isInstrument)
        return "Instrument";
    // CLAP says so; for the other formats a plugin with no audio at all is a MIDI effect.
    if (d.category.startsWith("Note Effect") || (d.numInputChannels == 0 && d.numOutputChannels == 0))
        return "MIDI Effect";
    return "Audio Effect";
}

juce::String groupOf(const juce::PluginDescription& d, PluginGrouping grouping)
{
    switch (grouping)
    {
    case PluginGrouping::vendor: return d.manufacturerName.trim().isNotEmpty() ? d.manufacturerName.trim() : "Unknown Vendor";
    case PluginGrouping::format: return d.pluginFormatName;
    case PluginGrouping::type: return typeOf(d) + "s";
    }
    return {};
}

PluginFile::Status statusOf(scan::PluginScanner::Outcome outcome)
{
    using O = scan::PluginScanner::Outcome;
    switch (outcome)
    {
    case O::found: return PluginFile::Status::ok;
    case O::noPlugins: return PluginFile::Status::noPlugins;
    case O::failed: return PluginFile::Status::failed;
    case O::crashed: return PluginFile::Status::crashed;
    case O::hung: return PluginFile::Status::hung;
    }
    return PluginFile::Status::failed;
}
} // namespace

PluginLibrary::PluginLibrary(library::LibraryIndex& i, juce::File scanner, juce::File work)
    : juce::Thread("Plugin scan"), index(i), scannerFile(std::move(scanner)), workFolder(std::move(work))
{
    {
        const std::lock_guard<std::mutex> hold(statusLock);
        state.plugins = static_cast<int>(index.storedPlugins().size());
    }
    startThread(juce::Thread::Priority::low);
}

PluginLibrary::~PluginLibrary()
{
    cancelRequested = true;
    signalThreadShouldExit();
    notify();
    stopThread(10000);
    cancelPendingUpdate();
}

juce::File PluginLibrary::scannerBesideApp()
{
#if JUCE_WINDOWS
    constexpr auto name = "LatticePluginScanner.exe";
#else
    constexpr auto name = "LatticePluginScanner";
#endif
    return juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile(name);
}

PluginSettings PluginLibrary::settingsCopy() const
{
    const std::lock_guard<std::mutex> hold(settingsLock);
    return current;
}

void PluginLibrary::setSettings(const PluginSettings& s, bool scanIfFoldersChanged)
{
    PluginSettings old;
    {
        const std::lock_guard<std::mutex> hold(settingsLock);
        old = current;
        current = s;
    }
    if (old.useDefaultFolders != s.useDefaultFolders || old.extraFolders != s.extraFolders)
    {
        if (scanIfFoldersChanged)
            scan();
    }
    else if (old.grouping != s.grouping || old.preferredFormat != s.preferredFormat)
    {
        const std::lock_guard<std::mutex> hold(requestLock);
        rebuildRequested = true;
        busy = true;
        notify();
    }
}

void PluginLibrary::scan(bool everything, int delayMs)
{
    {
        const std::lock_guard<std::mutex> hold(requestLock);
        requested = true;
        requestEverything = requestEverything || everything;
        requestDelayMs = juce::jmax(requestDelayMs, delayMs);
        busy = true;
    }
    notify();
}

void PluginLibrary::retry(const std::vector<PluginFile>& files)
{
    {
        const std::lock_guard<std::mutex> hold(requestLock);
        for (const auto& f : files)
            retries.insert({f.format, f.identifier});
        requested = true;
        busy = true;
    }
    notify();
}

void PluginLibrary::cancel()
{
    cancelRequested = true;
}

PluginLibrary::Status PluginLibrary::status() const
{
    const std::lock_guard<std::mutex> hold(statusLock);
    return state;
}

std::vector<PluginFile> PluginLibrary::problems() const
{
    std::vector<PluginFile> result;
    for (auto& f : index.pluginFiles())
        if (f.blocked() || f.status == PluginFile::Status::failed)
            result.push_back(std::move(f));
    return result;
}

bool PluginLibrary::waitForTests(int timeoutMs)
{
    const auto until = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
    while (busy.load())
    {
        if (juce::Time::getMillisecondCounter() > until)
            return false;
        juce::Thread::sleep(5);
    }
    handleUpdateNowIfNeeded();
    return true;
}

juce::FileSearchPath PluginLibrary::foldersFor(juce::AudioPluginFormat& format, const PluginSettings& settings)
{
    juce::FileSearchPath paths;
    if (settings.useDefaultFolders)
        paths = format.getDefaultLocationsToSearch();
    for (const auto& f : settings.extraFolders)
        if (f.trim().isNotEmpty())
            paths.addIfNotAlreadyThere(juce::File(f.trim()));
    return paths;
}

std::pair<juce::int64, juce::int64> PluginLibrary::stampOf(const juce::String& identifier)
{
    if (!juce::File::isAbsolutePath(identifier))
        return {0, 0}; // an identifier that's no file (an LV2 URI): unchanged while it's listed
    const juce::File f(identifier);
    if (f.existsAsFile())
        return {f.getSize(), f.getLastModificationTime().toMilliseconds()};
    if (!f.isDirectory())
        return {0, 0};
    // A bundle (VST3 on every OS, CLAP and AU on macOS): its files, but for the resources, which
    // can be large and don't change what the plugin is.
    juce::int64 size = 0, modified = f.getLastModificationTime().toMilliseconds();
    for (const auto& entry : juce::RangedDirectoryIterator(f, true, "*", juce::File::findFiles))
    {
        if (entry.getFile().getFullPathName().contains(juce::File::getSeparatorString() + juce::String("Resources") +
                                                       juce::File::getSeparatorString()))
            continue;
        size += entry.getFileSize();
        modified = juce::jmax(modified, entry.getModificationTime().toMilliseconds());
    }
    return {size, modified};
}

void PluginLibrary::run()
{
    while (!threadShouldExit())
    {
        bool everything = false, rebuildOnly = false;
        int delayMs = 0;
        std::set<std::pair<juce::String, juce::String>> retry;
        {
            std::unique_lock<std::mutex> hold(requestLock);
            if (!requested && !rebuildRequested)
            {
                busy = false;
                hold.unlock();
                wait(-1);
                continue;
            }
            rebuildOnly = !requested;
            everything = requestEverything;
            delayMs = requestDelayMs;
            retry = std::move(retries);
            retries.clear();
            requested = requestEverything = rebuildRequested = false;
            requestDelayMs = 0;
        }
        if (rebuildOnly)
        {
            rebuildItems();
            triggerAsyncUpdate();
            continue;
        }
        if (delayMs > 0 && wait(delayMs) && threadShouldExit())
            break;
        if (threadShouldExit())
            break;
        cancelRequested = false;
        {
            const std::lock_guard<std::mutex> hold(statusLock);
            state.scanning = true;
            state.done = state.total = 0;
            state.lastError.clear();
        }
        triggerAsyncUpdate();
        scanPass(everything, retry);
        {
            const std::lock_guard<std::mutex> hold(statusLock);
            state.scanning = false;
            state.plugins = static_cast<int>(index.storedPlugins().size());
        }
        triggerAsyncUpdate();
    }
    busy = false;
}

void PluginLibrary::scanPass(bool everything, const std::set<std::pair<juce::String, juce::String>>& retry)
{
    const auto settings = settingsCopy();

    // What's on the disk now.
    struct Candidate
    {
        scan::Job job;
        juce::int64 size = 0, modified = 0;
    };
    std::vector<Candidate> candidates;
    {
        juce::AudioPluginFormatManager formats;
        addHostedFormats(formats);
        for (auto* format : formats.getFormats())
        {
            if (threadShouldExit() || cancelRequested)
                return;
            // Once each: LV2 lists a bundle once for every plugin in it.
            std::set<juce::String> seen;
            for (const auto& id : format->searchPathsForPlugins(foldersFor(*format, settings), true, false))
            {
                if (!seen.insert(id).second)
                    continue;
                const auto [size, modified] = stampOf(id);
                candidates.push_back({{format->getName(), id}, size, modified});
            }
        }
    }

    // What changed since the last scan.
    std::map<std::pair<juce::String, juce::String>, PluginFile> known;
    for (auto& f : index.pluginFiles())
        known[{f.format, f.identifier}] = std::move(f);
    std::vector<scan::Job> jobs;
    std::map<std::pair<juce::String, juce::String>, const Candidate*> byJob;
    std::set<std::pair<juce::String, juce::String>> present;
    for (const auto& c : candidates)
    {
        const std::pair<juce::String, juce::String> key{c.job.format, c.job.identifier};
        present.insert(key);
        const auto k = known.find(key);
        const bool changed = k == known.end() || k->second.size != c.size || k->second.modified != c.modified;
        const bool blocked = k != known.end() && k->second.blocked();
        if (changed || retry.count(key) > 0 || (everything && !blocked))
        {
            jobs.push_back(c.job);
            byJob[key] = &c;
        }
    }
    std::vector<std::pair<juce::String, juce::String>> gone;
    for (const auto& [key, file] : known)
        if (present.count(key) == 0)
            gone.push_back(key);
    index.removePluginFiles(gone);
    if (!gone.empty() || jobs.empty())
        rebuildItems();
    {
        const std::lock_guard<std::mutex> hold(statusLock);
        state.total = static_cast<int>(jobs.size());
    }
    if (jobs.empty())
        return;
    triggerAsyncUpdate();

    scan::PluginScanner scanner({scannerFile, workFolder, settings.timeLimitSeconds});
    auto lastRebuild = juce::Time::getMillisecondCounter();
    const bool finished = scanner.run(
        jobs,
        [&](const scan::PluginScanner::Result& r) {
            const auto* c = byJob[{r.job.format, r.job.identifier}];
            PluginFile file;
            file.format = r.job.format;
            file.identifier = r.job.identifier;
            file.size = c != nullptr ? c->size : 0;
            file.modified = c != nullptr ? c->modified : 0;
            file.status = statusOf(r.outcome);
            file.reason = r.reason;
            file.scanned = juce::Time::currentTimeMillis();
            std::vector<library::StoredPlugin> stored;
            for (const auto& p : r.plugins)
                stored.push_back({p.key(), file.format, file.identifier,
                                  p.toXml()->toString(juce::XmlElement::TextFormat().singleLine().withoutHeader())});
            index.recordPluginFile(file, stored);
            {
                const std::lock_guard<std::mutex> hold(statusLock);
                ++state.done;
            }
            // A long first scan shows what it has found as it goes.
            if (juce::Time::getMillisecondCounter() - lastRebuild > 1000)
            {
                rebuildItems();
                lastRebuild = juce::Time::getMillisecondCounter();
            }
            triggerAsyncUpdate();
        },
        [this] { return threadShouldExit() || cancelRequested.load(); });
    if (!finished)
    {
        const std::lock_guard<std::mutex> hold(statusLock);
        state.lastError = scanner.error().isNotEmpty() ? scanner.error() : juce::String("Cancelled");
    }
    rebuildItems();
}

void PluginLibrary::rebuildItems()
{
    const auto settings = settingsCopy();
    auto order = hostedFormatNames();
    if (settings.preferredFormat.isNotEmpty() && order.contains(settings.preferredFormat))
    {
        order.removeString(settings.preferredFormat);
        order.insert(0, settings.preferredFormat);
    }
    const auto rank = [&](const juce::String& format) {
        const int i = order.indexOf(format);
        return i >= 0 ? i : order.size();
    };

    // One of each plugin: the same name and vendor in several formats is the same plugin.
    std::map<std::pair<juce::String, juce::String>, scan::ScannedPlugin> chosen;
    for (const auto& stored : index.storedPlugins())
    {
        const auto xml = juce::parseXML(stored.xml);
        if (xml == nullptr)
            continue;
        auto plugin = scan::ScannedPlugin::fromXml(*xml);
        if (!plugin)
            continue;
        const auto& d = plugin->description;
        const std::pair<juce::String, juce::String> same{d.name.toLowerCase(), d.manufacturerName.trim().toLowerCase()};
        const auto at = chosen.find(same);
        if (at == chosen.end() || rank(d.pluginFormatName) < rank(at->second.description.pluginFormatName))
            chosen[same] = std::move(*plugin);
    }

    std::vector<Item> items;
    std::set<juce::String> groups;
    const auto groupKey = [&](const juce::String& group) {
        return "plugins:" + juce::String(static_cast<int>(settings.grouping)) + ":" + group;
    };
    for (const auto& [same, plugin] : chosen)
    {
        const auto& d = plugin.description;
        const auto group = groupOf(d, settings.grouping);
        groups.insert(group);
        Item item;
        item.kind = library::Kind::plugin;
        item.key = "plugin:" + plugin.key();
        item.name = d.name;
        item.deviceType = "plugin";
        item.folder = groupKey(group);
        item.modified = d.lastFileModTime.toMilliseconds();
        juce::StringArray detail;
        if (d.manufacturerName.trim().isNotEmpty())
            detail.add(d.manufacturerName.trim());
        detail.add(d.pluginFormatName);
        detail.add(typeOf(d));
        item.detail = detail.joinIntoString(" \xc2\xb7 ");
        items.push_back(std::move(item));
    }
    for (const auto& group : groups)
    {
        Item folder;
        folder.kind = library::Kind::folder;
        folder.key = groupKey(group);
        folder.name = group;
        items.push_back(std::move(folder));
    }
    index.setPluginItems(items);
}

void PluginLibrary::handleAsyncUpdate()
{
    sendChangeMessage();
}

} // namespace lattice::hosting
