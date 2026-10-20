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

    Idle workers spin briefly (waking them costs almost nothing while blocks come quickly), then
    sleep until the next job. They run at real-time priority (on Windows also in the "Pro Audio"
    MMCSS class). Every task writes only its own data; the caller combines results in a fixed
    order afterwards, so the output doesn't depend on which thread ran what. */
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

private:
    class Worker;
    friend class Worker;

    /** Takes and runs tasks of the current job until there are none left. */
    void work() const noexcept LATTICE_NONBLOCKING;

    static constexpr int spinIterations = 4000;

    // The current job (written before the ticket that publishes it). Mutable: run() is const, so
    // the pool can be shared read-only (RtShared) while it works.
    mutable Task jobTask = nullptr;
    mutable void* jobContext = nullptr;
    // High 32 bits: the job's task count (0 between jobs); low 32 bits: the next task index.
    alignas(64) mutable std::atomic<std::uint64_t> ticket{0};
    alignas(64) mutable std::atomic<int> done{0};
    alignas(64) mutable std::atomic<std::uint32_t> wake{0}; // bumped for every job: sleepers wait on it
    mutable std::atomic<int> sleepers{0};
    std::atomic<bool> stopping{false};
    std::vector<std::unique_ptr<Worker>> threads;
};

} // namespace lattice
