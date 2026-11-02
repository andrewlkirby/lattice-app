#include "platform/FileWatcher.h"
#include "platform/FileWatcherChanges.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <atomic>
#include <map>
#include <thread>

namespace lattice::platform
{

/** inotify watches one folder at a time, so every folder below a watched one gets its own watch,
    and new folders get theirs as they appear. One thread reads the events; an eventfd wakes it to
    stop. */
class FileWatcher::Impl
{
public:
    explicit Impl(FileWatcher& owner) : changes(owner)
    {
        fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
        wake = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (fd >= 0 && wake >= 0)
            thread = std::thread([this] { run(); });
    }

    ~Impl()
    {
        stopping = true;
        if (wake >= 0)
        {
            const std::uint64_t one = 1;
            [[maybe_unused]] const auto written = ::write(wake, &one, sizeof(one));
        }
        if (thread.joinable())
            thread.join();
        if (fd >= 0)
            ::close(fd);
        if (wake >= 0)
            ::close(wake);
    }

    void watch(const juce::File& folder)
    {
        if (fd < 0 || !folder.isDirectory())
            return;
        const std::lock_guard<std::mutex> hold(lock);
        if (roots.contains(folder))
            return;
        roots.add(folder);
        addTree(folder);
    }

    void unwatch(const juce::File& folder)
    {
        const std::lock_guard<std::mutex> hold(lock);
        if (!roots.contains(folder))
            return;
        roots.removeFirstMatchingValue(folder);
        for (auto it = folders.begin(); it != folders.end();)
        {
            const auto& path = it->second;
            // Still needed if another watched folder holds it.
            bool kept = false;
            for (const auto& r : roots)
                kept = kept || path == r || path.isAChildOf(r);
            if (!kept && (path == folder || path.isAChildOf(folder)))
            {
                inotify_rm_watch(fd, it->first);
                it = folders.erase(it);
            }
            else
                ++it;
        }
    }

    juce::Array<juce::File> watched() const
    {
        const std::lock_guard<std::mutex> hold(lock);
        return roots;
    }

    detail::ChangedFolders changes;

private:
    void addOne(const juce::File& folder) // lock held
    {
        constexpr std::uint32_t mask =
            IN_CREATE | IN_DELETE | IN_MOVED_FROM | IN_MOVED_TO | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR;
        const int wd = inotify_add_watch(fd, folder.getFullPathName().toRawUTF8(), mask);
        if (wd >= 0)
            folders[wd] = folder;
    }

    void addTree(const juce::File& folder) // lock held
    {
        addOne(folder);
        for (const auto& entry : juce::RangedDirectoryIterator(folder, true, "*", juce::File::findDirectories,
                                                                juce::File::FollowSymlinks::noCycles))
            addOne(entry.getFile());
    }

    void run()
    {
        alignas(inotify_event) char buffer[16 * 1024];
        pollfd fds[2] = {{fd, POLLIN, 0}, {wake, POLLIN, 0}};
        while (!stopping)
        {
            if (poll(fds, 2, -1) < 0 || stopping)
                continue;
            if ((fds[0].revents & POLLIN) == 0)
                continue;
            for (;;)
            {
                const auto n = ::read(fd, buffer, sizeof(buffer));
                if (n <= 0)
                    break;
                const std::lock_guard<std::mutex> hold(lock);
                for (char* p = buffer; p < buffer + n;)
                {
                    const auto* e = reinterpret_cast<const inotify_event*>(p);
                    p += sizeof(inotify_event) + e->len;
                    if (e->mask & IN_Q_OVERFLOW)
                    {
                        for (const auto& r : roots)
                            changes.add(r);
                        continue;
                    }
                    const auto found = folders.find(e->wd);
                    if (found == folders.end())
                        continue;
                    const auto folder = found->second;
                    if (e->mask & IN_IGNORED)
                    {
                        folders.erase(found);
                        continue;
                    }
                    if (e->mask & (IN_DELETE_SELF | IN_MOVE_SELF))
                    {
                        changes.add(folder.getParentDirectory());
                        continue;
                    }
                    if ((e->mask & IN_ISDIR) && (e->mask & (IN_CREATE | IN_MOVED_TO)) && e->len > 0)
                        addTree(folder.getChildFile(e->name));
                    changes.add(folder);
                }
            }
        }
    }

    int fd = -1;
    int wake = -1;
    mutable std::mutex lock;
    juce::Array<juce::File> roots;             // guarded by lock
    std::map<int, juce::File> folders;         // watch descriptor -> folder; guarded by lock
    std::atomic<bool> stopping{false};
    std::thread thread;
};

FileWatcher::FileWatcher() : impl(std::make_unique<Impl>(*this)) {}
FileWatcher::~FileWatcher() = default;
void FileWatcher::watch(const juce::File& folder) { impl->watch(folder); }
void FileWatcher::unwatch(const juce::File& folder) { impl->unwatch(folder); }
juce::Array<juce::File> FileWatcher::watched() const { return impl->watched(); }
std::vector<juce::File> FileWatcher::takeChanges() { return impl->changes.take(); }

} // namespace lattice::platform
