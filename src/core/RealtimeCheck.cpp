#include "core/RealtimeCheck.h"

#include <juce_core/juce_core.h>

#include <atomic>

namespace lattice::rt
{
namespace
{
// Trivially-initialised thread_locals: reading them never allocates or takes a lock.
constinit thread_local bool tlsInAudioThread = false;
constinit thread_local bool tlsInViolationHandler = false;
constinit thread_local AllocationCounters tlsCounters{};

std::atomic<bool> gHooksInstalled{false};
std::atomic<std::uint64_t> gViolations{0};

void defaultViolationHandler(const char* what, std::size_t bytes)
{
    juce::ignoreUnused(what, bytes);
    // An allocation, deallocation, or blocking call happened on the audio thread. Look at the call
    // stack: the offending code must be moved off the audio thread or given a pre-allocated buffer.
    jassertfalse;
}

std::atomic<ViolationHandler> gHandler{&defaultViolationHandler};
} // namespace

bool isAudioThread() noexcept
{
    return tlsInAudioThread;
}

ScopedAudioThread::ScopedAudioThread() noexcept : previous(tlsInAudioThread)
{
    tlsInAudioThread = true;
}

ScopedAudioThread::~ScopedAudioThread() noexcept
{
    tlsInAudioThread = previous;
}

AllocationCounters countersForThisThread() noexcept
{
    return tlsCounters;
}

bool hooksInstalled() noexcept
{
    return gHooksInstalled.load(std::memory_order_relaxed);
}

std::uint64_t violationCount() noexcept
{
    return gViolations.load(std::memory_order_relaxed);
}

ViolationHandler setViolationHandler(ViolationHandler handler) noexcept
{
    return gHandler.exchange(handler != nullptr ? handler : &defaultViolationHandler);
}

void reportViolation(const char* what, std::size_t bytes) noexcept
{
    gViolations.fetch_add(1, std::memory_order_relaxed);

    if (tlsInViolationHandler)
        return;

    // The handler may allocate (logging, assertions). Those allocations are not violations.
    tlsInViolationHandler = true;
    const bool wasAudio = tlsInAudioThread;
    tlsInAudioThread = false;
    gHandler.load()(what, bytes);
    tlsInAudioThread = wasAudio;
    tlsInViolationHandler = false;
}

namespace detail
{
void markHooksInstalled() noexcept
{
    gHooksInstalled.store(true, std::memory_order_relaxed);
}

void noteAllocation(std::size_t bytes) noexcept
{
    ++tlsCounters.allocations;
    tlsCounters.bytesAllocated += bytes;
    if (tlsInAudioThread)
        reportViolation("allocation on the audio thread", bytes);
}

void noteDeallocation() noexcept
{
    ++tlsCounters.deallocations;
    if (tlsInAudioThread)
        reportViolation("deallocation on the audio thread");
}
} // namespace detail

} // namespace lattice::rt
