#pragma once

#include "core/RealtimeCheck.h"

#include <juce_core/juce_core.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace lattice
{

/** Real-time worker threads for parallel track processing (docs/04-m3-devices.md, "Parallel track
    processing").

    The audio thread hands out a job of `count` independent tasks with run(); the workers and the
    audio thread itself take tasks until all are done, so a small job never waits on another
    thread. Tasks are claimed through one atomic ticket that carries the job's task count next to
    the next task's index, so a claim is checked against the job it was taken from: a worker that
    comes back late (preempted between jobs) can never run a task of the next job by mistake.

    Workers are woken only for jobs with real work in them (wakeAfterNanoseconds of task time,
    summed over the threads): at once when recent jobs had that much, otherwise once the audio
    thread has spent that long on this one (it starts on it meanwhile). A quiet set's jobs
    (silence, the transport stopped) take microseconds, so the workers stay asleep and an idle app
    costs nothing (charter §1: idle CPU). While jobs have work and come quickly (at most
    maxSpinNanoseconds apart: small buffers), a worker stays awake between them, as waking takes a
    sleeping thread over 100 us, a large share of a short period; with longer periods it sleeps,
    and the wake costs a small share. They run at real-time priority (on Windows also in the
    "Pro Audio" MMCSS class). Every task writes only its own data; the caller combines results in a fixed
    order afterwards, so the output doesn't depend on which thread ran what.

    Each thread's time in tasks is counted (two clock reads per task), for the load meter's time
    per worker. */
class WorkerPool
{
public:
    using Task = void (*)(void* context, int index) noexcept;

    /** Message thread. 0 workers: everything runs on the calling thread. */
    explicit WorkerPool(int numWorkers);
    ~WorkerPool();

    WorkerPool(const WorkerPool&) = delete;
    WorkerPool& operator=(const WorkerPool&) = delete;

    int numWorkers() const noexcept { return static_cast<int>(threads.size()); }

    /** The default: one worker per physical core, minus one for the audio thread. */
    static int defaultWorkers();

    /** Audio thread: runs task(context, i) for i in [0, count), and returns once all are done. */
    void run(int count, Task task, void* context) const noexcept LATTICE_NONBLOCKING;

    /** Any thread: how long each thread has spent running tasks since the pool started, in
        nanoseconds: [0] the audio thread (run()'s caller), then each worker. Empty with no workers
        (everything runs on the audio thread, inside its callback time). */
    std::vector<std::uint64_t> busyNanoseconds() const;

private:
    class Worker;
    friend class Worker;

    /** A worker: takes and runs tasks of the current job until there are none left. thread: 1 +
        the worker's index (whose time it adds to). */
    void work(int thread) const noexcept LATTICE_NONBLOCKING;

    /** A job with this much task time is worth waking the sleeping workers for. */
    static constexpr std::uint64_t wakeAfterNanoseconds = 100'000;
    /** Jobs at most this far apart keep the workers awake between them (with a quarter more as a
        margin before a worker gives up and sleeps). */
    static constexpr std::uint64_t maxSpinNanoseconds = 3'000'000;

    // The current job (written before the ticket that publishes it). Mutable: run() is const, so
    // the pool can be shared read-only (RtShared) while it works.
    mutable Task jobTask = nullptr;
    mutable void* jobContext = nullptr;
    // High 32 bits: the job's task count (0 between jobs); low 32 bits: the next task index.
    alignas(64) mutable std::atomic<std::uint64_t> ticket{0};
    alignas(64) mutable std::atomic<int> done{0};
    alignas(64) mutable std::atomic<std::uint32_t> wake{0}; // bumped for every job: sleepers wait on it
    mutable std::atomic<int> sleepers{0};
    // Written by the audio thread after each job, read by the workers: the recent jobs' largest
    // task time (summed over the threads) and largest time from one job's start to the next. A
    // block has several jobs (the tracks, each level of groups, the returns), so these are maxima
    // that decay by a sixteenth a job: they hold a block's biggest job and its period.
    mutable std::atomic<std::uint64_t> recentWork{0};
    mutable std::atomic<std::uint64_t> recentInterval{0};
    mutable std::uint64_t lastJobStart = 0, busyTotal = 0; // the audio thread's own
    std::atomic<bool> stopping{false};
    struct alignas(64) Busy // one cache line each: every thread writes its own
    {
        std::atomic<std::uint64_t> nanoseconds{0};
    };
    std::unique_ptr<Busy[]> busy; // numWorkers + 1
    std::vector<std::unique_ptr<Worker>> threads;
};

} // namespace lattice
