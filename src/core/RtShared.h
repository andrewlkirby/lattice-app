#pragma once

#include "core/RealtimeCheck.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <utility>
#include <vector>

namespace lattice
{

/** Hands immutable objects from the message thread to real-time readers without locks.

    - The message thread publishes a new object with publish(). The previous one is "retired",
      not freed, because a reader may still be using it.
    - Readers open a ReadScope, call get(), and use the pointer only while the scope is open. A
      scope costs two atomic increments; it never locks, allocates, or frees.
    - collect() (message thread, e.g. on a timer and after each publish) frees retired objects once
      no reader scope that could have seen them is still open: any moment after the retirement at
      which the count of open scopes is zero proves that.

    Readers may be on several threads (the M3 parallel engine). With readers that overlap without
    a single gap, retired objects are only held longer; it is never unsafe. */
template <typename T>
class RtShared
{
public:
    RtShared() = default;
    explicit RtShared(std::shared_ptr<const T> initial) { publish(std::move(initial)); }

    ~RtShared()
    {
        // Owners must stop all readers before destruction.
        jassert(activeReaders.load() == 0);
    }

    RtShared(const RtShared&) = delete;
    RtShared& operator=(const RtShared&) = delete;

    /** Message thread. */
    void publish(std::shared_ptr<const T> next)
    {
        RT_ASSERT_NOT_AUDIO_THREAD();
        current.store(next.get(), std::memory_order_seq_cst);
        if (owner != nullptr)
            retired.push_back(std::move(owner));
        owner = std::move(next);
        collect();
    }

    /** Message thread. The published object (the owning pointer). */
    const std::shared_ptr<const T>& published() const noexcept { return owner; }

    /** Message thread. Frees retired objects that no reader can still hold. */
    void collect()
    {
        RT_ASSERT_NOT_AUDIO_THREAD();
        if (retired.empty())
            return;

        // seq_cst on both sides: a reader that opened its scope before this load is counted here;
        // one that opens later loads `current` afterwards and so sees the new object.
        if (activeReaders.load(std::memory_order_seq_cst) == 0)
            retired.clear();
    }

    /** Message thread. Number of retired objects still waiting to be freed (for tests). */
    std::size_t retiredCount() const noexcept { return retired.size(); }

    class ReadScope
    {
    public:
        explicit ReadScope(const RtShared& s) noexcept : shared(s)
        {
            shared.activeReaders.fetch_add(1, std::memory_order_seq_cst);
        }
        ~ReadScope() { shared.activeReaders.fetch_sub(1, std::memory_order_seq_cst); }
        ReadScope(const ReadScope&) = delete;
        ReadScope& operator=(const ReadScope&) = delete;

        /** The current object, valid until this scope closes. May be nullptr. */
        const T* get() const noexcept { return shared.current.load(std::memory_order_seq_cst); }

    private:
        const RtShared& shared;
    };

private:
    std::atomic<const T*> current{nullptr};
    mutable std::atomic<int> activeReaders{0};
    std::shared_ptr<const T> owner;   // message thread only
    std::vector<std::shared_ptr<const T>> retired; // message thread only
};

} // namespace lattice
