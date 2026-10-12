#pragma once

#include "core/RealtimeCheck.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace lattice
{

/** Hands immutable objects from the message thread to one real-time reader that may keep using an
    older object for a while after a newer one is published (docs/04-m3-devices.md, "Chain changes").

    RtShared frees a retired object as soon as no read scope is open, which is right for objects
    used within one block. A track's device chain is different: when the chain changes, the audio
    thread fades the old chain out over several blocks before it switches to the new one. Here the
    reader says explicitly which version it has settled on (settle()), and only versions older than
    that are freed.

    - publish() (message thread) makes a new version current. Versions get increasing generations.
    - The reader (one thread) calls latest() to see the newest version, and settle() once it no
      longer uses anything older than a given version.
    - collect() (message thread) frees versions older than the settled one. The latest version is
      never freed.
    - resetReader() (message thread, only while the reader is not running, e.g. while the audio
      device is stopped) settles on the latest version, so nothing waits for a reader that isn't
      reading. */
template <typename T>
class SettledShared
{
public:
    struct Version
    {
        std::shared_ptr<const T> object;
        std::uint64_t generation = 0;
    };

    SettledShared() = default;
    SettledShared(const SettledShared&) = delete;
    SettledShared& operator=(const SettledShared&) = delete;

    /** Message thread. */
    void publish(std::shared_ptr<const T> next)
    {
        RT_ASSERT_NOT_AUDIO_THREAD();
        auto version = std::make_unique<Version>();
        version->object = std::move(next);
        version->generation = ++lastGeneration;
        latestVersion.store(version.get(), std::memory_order_release);
        versions.push_back(std::move(version));
        collect();
    }

    /** Message thread. The object published last (may be null). */
    std::shared_ptr<const T> published() const
    {
        return versions.empty() ? nullptr : versions.back()->object;
    }

    /** Message thread. Frees versions the reader has settled past. */
    void collect()
    {
        RT_ASSERT_NOT_AUDIO_THREAD();
        const auto settledOn = settledGeneration.load(std::memory_order_acquire);
        // Keep the latest version, and every version at or after the settled one.
        const auto keepFrom = std::find_if(versions.begin(), versions.end() - (versions.empty() ? 0 : 1),
                                           [&](const auto& v) { return v->generation >= settledOn; });
        versions.erase(versions.begin(), keepFrom);
    }

    /** Message thread, while the reader is not running. */
    void resetReader()
    {
        RT_ASSERT_NOT_AUDIO_THREAD();
        settledGeneration.store(lastGeneration, std::memory_order_release);
        collect();
    }

    /** Message thread. Versions not yet freed, including the latest (for tests). */
    std::size_t versionCount() const noexcept { return versions.size(); }

    // --- Reader -------------------------------------------------------------------------------------
    /** The newest version, or nullptr before the first publish. */
    const Version* latest() const noexcept LATTICE_NONBLOCKING
    {
        return latestVersion.load(std::memory_order_acquire);
    }

    /** The reader uses nothing older than `version` from now on. */
    void settle(const Version* version) noexcept LATTICE_NONBLOCKING
    {
        if (version != nullptr)
            settledGeneration.store(version->generation, std::memory_order_release);
    }

private:
    std::atomic<const Version*> latestVersion{nullptr};
    std::atomic<std::uint64_t> settledGeneration{0};
    std::vector<std::unique_ptr<Version>> versions; // message thread only, oldest first
    std::uint64_t lastGeneration = 0;
};

} // namespace lattice
