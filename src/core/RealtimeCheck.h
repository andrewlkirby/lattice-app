#pragma once

// Real-time safety rules, enforced from day one (docs/09-performance-charter.md §4):
// no allocation, locks, or blocking calls on the audio thread.
//
// - ScopedAudioThread marks the current thread as "audio" for its lifetime. The audio callback
//   opens one at its very top.
// - When the allocation hooks (src/core/AllocationHooks.cpp) are linked in, every operator
//   new/delete made while the flag is set is counted as a violation and reported.
// - RT_ASSERT_NOT_AUDIO_THREAD() goes into code that takes locks or blocks.
// - LATTICE_NONBLOCKING marks audio-thread entry points for Clang's function-effect analysis and
//   RTSan (-fsanitize=realtime). It expands to nothing on other compilers.

#include <cstddef>
#include <cstdint>

#if defined(__clang__) && defined(__has_cpp_attribute)
#if __has_cpp_attribute(clang::nonblocking)
#define LATTICE_NONBLOCKING [[clang::nonblocking]]
#endif
#endif
#ifndef LATTICE_NONBLOCKING
#define LATTICE_NONBLOCKING
#endif

// LATTICE_RT_NONBLOCKING_SYSCALL: opens a scope for a system call the audio thread may make
// because it never waits: waking sleeping worker threads (a futex wake / WakeByAddressAll).
// RTSan can't tell a wake from a wait, so the scope switches it off; use it for nothing else.
#if defined(__has_feature)
#if __has_feature(realtime_sanitizer)
#include <sanitizer/rtsan_interface.h>
#define LATTICE_RT_NONBLOCKING_SYSCALL const __rtsan::ScopedDisabler latticeNonBlockingSyscall
#endif
#endif
#ifndef LATTICE_RT_NONBLOCKING_SYSCALL
#define LATTICE_RT_NONBLOCKING_SYSCALL static_cast<void>(0)
#endif

namespace lattice::rt
{

/** True while the calling thread is inside a ScopedAudioThread. */
bool isAudioThread() noexcept;

/** Marks the calling thread as the audio thread for the guard's lifetime. Nests correctly. */
class ScopedAudioThread
{
public:
    ScopedAudioThread() noexcept;
    ~ScopedAudioThread() noexcept;
    ScopedAudioThread(const ScopedAudioThread&) = delete;
    ScopedAudioThread& operator=(const ScopedAudioThread&) = delete;

private:
    bool previous;
};

/** Per-thread allocation counters, updated by the allocation hooks. All zero if the hooks are not
    linked into this binary (see hooksInstalled()). */
struct AllocationCounters
{
    std::uint64_t allocations = 0;
    std::uint64_t deallocations = 0;
    std::uint64_t bytesAllocated = 0;
};

AllocationCounters countersForThisThread() noexcept;

/** True when this binary contains the replacement operator new/delete. */
bool hooksInstalled() noexcept;

/** Total allocations/deallocations made on any thread while it was marked as the audio thread. */
std::uint64_t violationCount() noexcept;

/** Called on every violation, on the offending thread, with allocation hooks suspended so it may
    allocate. The default handler raises a JUCE assertion (Debug builds stop in the debugger). */
using ViolationHandler = void (*)(const char* what, std::size_t bytes);
ViolationHandler setViolationHandler(ViolationHandler handler) noexcept;

/** Reports a violation from code that detects one itself (e.g. a lock taken on the audio thread). */
void reportViolation(const char* what, std::size_t bytes = 0) noexcept;

namespace detail
{
// Used only by AllocationHooks.cpp.
void markHooksInstalled() noexcept;
void noteAllocation(std::size_t bytes) noexcept;
void noteDeallocation() noexcept;
} // namespace detail

} // namespace lattice::rt

#define RT_ASSERT_NOT_AUDIO_THREAD()                                                                \
    do                                                                                              \
    {                                                                                               \
        if (::lattice::rt::isAudioThread())                                                         \
            ::lattice::rt::reportViolation("blocking call on the audio thread");                    \
    } while (false)
