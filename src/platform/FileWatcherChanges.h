#pragma once

// What every platform's FileWatcher::Impl shares: the folders that changed, collected on the
// watcher's thread and taken on any other.

#include "platform/FileWatcher.h"

#include <algorithm>
#include <mutex>

namespace lattice::platform::detail
{

class ChangedFolders
{
public:
    explicit ChangedFolders(FileWatcher& w) : owner(w) {}

    void add(const juce::File& folder)
    {
        {
            const std::lock_guard<std::mutex> hold(lock);
            if (std::find(folders.begin(), folders.end(), folder) != folders.end())
                return;
            folders.push_back(folder);
        }
        if (owner.onChange)
            owner.onChange();
    }

    std::vector<juce::File> take()
    {
        const std::lock_guard<std::mutex> hold(lock);
        return std::exchange(folders, {});
    }

private:
    FileWatcher& owner;
    std::mutex lock;
    std::vector<juce::File> folders;
};

} // namespace lattice::platform::detail
