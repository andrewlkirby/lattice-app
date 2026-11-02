#include "platform/FileWatcher.h"
#include "platform/FileWatcherChanges.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <thread>

namespace lattice::platform
{

/** macOS for now: every two seconds, each watched folder's contents (every file's size and date)
    are compared with what they were, and the folders that differ are reported. FSEvents can
    replace this when the macOS build is brought up; the interface stays the same. */
class FileWatcher::Impl
{
public:
    explicit Impl(FileWatcher& owner) : changes(owner)
    {
        thread = std::thread([this] { run(); });
    }

    ~Impl()
    {
        {
            const std::lock_guard<std::mutex> hold(lock);
            stopping = true;
        }
        wake.notify_all();
        thread.join();
    }

    void watch(const juce::File& folder)
    {
        if (!folder.isDirectory())
            return;
        const std::lock_guard<std::mutex> hold(lock);
        if (!roots.contains(folder))
            roots.add(folder);
    }

    void unwatch(const juce::File& folder)
    {
        const std::lock_guard<std::mutex> hold(lock);
        roots.removeFirstMatchingValue(folder);
        states.erase(folder.getFullPathName());
    }

    juce::Array<juce::File> watched() const
    {
        const std::lock_guard<std::mutex> hold(lock);
        return roots;
    }

    detail::ChangedFolders changes;

private:
    using FolderState = std::map<juce::String, std::int64_t>; // folder -> a hash of what's in it

    static FolderState read(const juce::File& root)
    {
        FolderState state;
        state[root.getFullPathName()] = 0;
        for (const auto& entry : juce::RangedDirectoryIterator(root, true, "*", juce::File::findFilesAndDirectories,
                                                                juce::File::FollowSymlinks::noCycles))
        {
            const auto& f = entry.getFile();
            auto& h = state[f.getParentDirectory().getFullPathName()];
            h = h * 31 + f.getFileName().hashCode64() + entry.getFileSize() * 7 +
                entry.getModificationTime().toMilliseconds();
            if (entry.isDirectory())
                state.try_emplace(f.getFullPathName(), 0);
        }
        return state;
    }

    void run()
    {
        std::unique_lock<std::mutex> hold(lock);
        while (!stopping)
        {
            const auto now = roots;
            hold.unlock();
            for (const auto& root : now)
            {
                auto next = read(root);
                std::vector<juce::File> differ;
                {
                    const std::lock_guard<std::mutex> again(lock);
                    auto& before = states[root.getFullPathName()];
                    if (!before.empty())
                    {
                        for (const auto& [folder, h] : next)
                            if (auto b = before.find(folder); b == before.end() || b->second != h)
                                differ.emplace_back(folder);
                        for (const auto& [folder, h] : before)
                            if (next.find(folder) == next.end())
                                differ.emplace_back(juce::File(folder).getParentDirectory());
                    }
                    before = std::move(next);
                }
                for (const auto& f : differ)
                    changes.add(f);
            }
            hold.lock();
            wake.wait_for(hold, std::chrono::seconds(2), [this] { return stopping; });
        }
    }

    mutable std::mutex lock;
    std::condition_variable wake;
    juce::Array<juce::File> roots;               // guarded by lock
    std::map<juce::String, FolderState> states;  // guarded by lock
    bool stopping = false;                       // guarded by lock
    std::thread thread;
};

FileWatcher::FileWatcher() : impl(std::make_unique<Impl>(*this)) {}
FileWatcher::~FileWatcher() = default;
void FileWatcher::watch(const juce::File& folder) { impl->watch(folder); }
void FileWatcher::unwatch(const juce::File& folder) { impl->unwatch(folder); }
juce::Array<juce::File> FileWatcher::watched() const { return impl->watched(); }
std::vector<juce::File> FileWatcher::takeChanges() { return impl->changes.take(); }

} // namespace lattice::platform
