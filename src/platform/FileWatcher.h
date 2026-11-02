#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <memory>
#include <vector>

namespace lattice::platform
{

/** Watches folders, and everything below them, for files being added, removed, renamed or
    changed (docs/05-m4-ui.md, 4e: the library rescans what changed, so new files appear without a
    restart). ReadDirectoryChangesW on Windows, inotify on Linux, and on macOS a comparison of the
    folders' contents every two seconds.

    Changes are collected on the watcher's own thread: takeChanges() hands over the folders where
    something changed (each once), and onChange says there are some. A folder that is renamed or
    removed shows up as a change in its parent. */
class FileWatcher
{
public:
    FileWatcher();
    ~FileWatcher();
    FileWatcher(const FileWatcher&) = delete;
    FileWatcher& operator=(const FileWatcher&) = delete;

    /** Any thread. Watching a folder twice, or one that doesn't exist, does nothing. */
    void watch(const juce::File& folder);
    void unwatch(const juce::File& folder);
    juce::Array<juce::File> watched() const;

    /** Any thread: the folders where something changed since the last call. */
    std::vector<juce::File> takeChanges();

    /** Called on the watcher's thread when changes are waiting (set it before watching). */
    std::function<void()> onChange;

    class Impl;

private:
    std::unique_ptr<Impl> impl;
};

} // namespace lattice::platform
