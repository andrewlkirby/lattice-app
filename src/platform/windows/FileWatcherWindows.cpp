#include "platform/FileWatcher.h"
#include "platform/FileWatcherChanges.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <thread>

namespace lattice::platform
{

/** One thread waits on every watched folder's ReadDirectoryChangesW (overlapped, watching the
    whole tree) and on an event that wakes it when the list of folders changes or it should stop. */
class FileWatcher::Impl
{
public:
    explicit Impl(FileWatcher& owner) : changes(owner)
    {
        wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        thread = std::thread([this] { run(); });
    }

    ~Impl()
    {
        stopping = true;
        SetEvent(wake);
        thread.join();
        for (auto& w : watches)
            close(*w);
        CloseHandle(wake);
    }

    void watch(const juce::File& folder)
    {
        if (!folder.isDirectory())
            return;
        const std::lock_guard<std::mutex> hold(lock);
        for (const auto& w : requested)
            if (w == folder)
                return;
        requested.add(folder);
        SetEvent(wake);
    }

    void unwatch(const juce::File& folder)
    {
        const std::lock_guard<std::mutex> hold(lock);
        requested.removeFirstMatchingValue(folder);
        SetEvent(wake);
    }

    juce::Array<juce::File> watched() const
    {
        const std::lock_guard<std::mutex> hold(lock);
        return requested;
    }

    detail::ChangedFolders changes;

private:
    struct Watch
    {
        juce::File folder;
        HANDLE directory = INVALID_HANDLE_VALUE;
        OVERLAPPED overlapped{};
        bool pending = false; // a read is waiting to complete
        alignas(DWORD) std::byte buffer[64 * 1024];
    };

    static bool start(Watch& w)
    {
        constexpr DWORD filter = FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                                 FILE_NOTIFY_CHANGE_LAST_WRITE;
        w.pending = ReadDirectoryChangesW(w.directory, w.buffer, sizeof(w.buffer), TRUE, filter, nullptr, &w.overlapped,
                                          nullptr) != 0;
        return w.pending;
    }

    static void close(Watch& w)
    {
        if (w.directory != INVALID_HANDLE_VALUE)
        {
            // Only a read still waiting needs cancelling and waiting for (with none, the wait never ends).
            if (w.pending)
            {
                CancelIoEx(w.directory, &w.overlapped);
                DWORD ignored = 0;
                GetOverlappedResult(w.directory, &w.overlapped, &ignored, TRUE);
                w.pending = false;
            }
            CloseHandle(w.directory);
        }
        if (w.overlapped.hEvent != nullptr)
            CloseHandle(w.overlapped.hEvent);
        w.directory = INVALID_HANDLE_VALUE;
        w.overlapped.hEvent = nullptr;
    }

    /** The watcher thread: brings the open watches in line with what's requested. */
    void sync()
    {
        juce::Array<juce::File> wanted;
        {
            const std::lock_guard<std::mutex> hold(lock);
            wanted = requested;
        }
        for (auto it = watches.begin(); it != watches.end();)
        {
            if (!wanted.contains((*it)->folder))
            {
                close(**it);
                it = watches.erase(it);
            }
            else
                ++it;
        }
        for (const auto& folder : wanted)
        {
            bool open = false;
            for (const auto& w : watches)
                open = open || w->folder == folder;
            // WaitForMultipleObjects takes 64 handles, one of them the wake event.
            if (open || watches.size() >= MAXIMUM_WAIT_OBJECTS - 1)
                continue;
            auto w = std::make_unique<Watch>();
            w->folder = folder;
            w->directory = CreateFileW(folder.getFullPathName().toWideCharPointer(), FILE_LIST_DIRECTORY,
                                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                       FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED, nullptr);
            if (w->directory == INVALID_HANDLE_VALUE)
                continue;
            w->overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!start(*w))
            {
                close(*w);
                continue;
            }
            watches.push_back(std::move(w));
        }
    }

    /** True if the watch carries on; false if its folder is gone (it's closed). */
    bool collect(Watch& w)
    {
        w.pending = false;
        DWORD bytes = 0;
        if (!GetOverlappedResult(w.directory, &w.overlapped, &bytes, FALSE))
            bytes = 0;
        if (bytes == 0)
        {
            changes.add(w.folder); // the buffer overflowed: look at the whole folder again
        }
        else
        {
            const auto* p = w.buffer;
            for (;;)
            {
                const auto* info = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(p);
                const juce::String relative(info->FileName, info->FileNameLength / sizeof(WCHAR));
                // The folder the change happened in.
                changes.add(w.folder.getChildFile(relative).getParentDirectory());
                if (info->NextEntryOffset == 0)
                    break;
                p += info->NextEntryOffset;
            }
        }
        ResetEvent(w.overlapped.hEvent);
        if (start(w))
            return true;
        // The folder was removed or renamed: say so, and stop watching it (watch() again restarts it).
        changes.add(w.folder.getParentDirectory());
        changes.add(w.folder);
        close(w);
        return false;
    }

    void run()
    {
        while (!stopping)
        {
            sync();
            std::vector<HANDLE> handles{wake};
            for (const auto& w : watches)
                handles.push_back(w->overlapped.hEvent);
            const DWORD r = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, INFINITE);
            if (stopping)
                break;
            if (r > WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + handles.size())
            {
                const auto at = watches.begin() + static_cast<std::ptrdiff_t>(r - WAIT_OBJECT_0 - 1);
                if (!collect(**at))
                    watches.erase(at); // still requested: sync() opens it again if it comes back
            }
        }
    }

    mutable std::mutex lock;
    juce::Array<juce::File> requested; // guarded by lock
    std::vector<std::unique_ptr<Watch>> watches; // the watcher thread's
    HANDLE wake = nullptr;
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
