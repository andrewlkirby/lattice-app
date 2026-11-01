#include "engine/WorkerPool.h"

#include "platform/Platform.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <chrono>
#include <thread>

#if defined(_M_X64) || defined(_M_AMD64) || defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#define LATTICE_CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(_M_ARM64)
#define LATTICE_CPU_PAUSE() __asm__ __volatile__("yield")
#else
#define LATTICE_CPU_PAUSE() ((void) 0)
#endif

namespace lattice
{

namespace
{
std::uint64_t nowNanoseconds() noexcept
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
} // namespace

class WorkerPool::Worker final : public juce::Thread
{
public:
    Worker(WorkerPool& p, int index) : juce::Thread("Lattice audio worker " + juce::String(index + 1)), pool(p), thread(index + 1) {}

    void run() override
    {
        platform::joinProAudioClass();
        // As on the audio thread (AudioEngine::process): the same flush-to-zero rules, so a
        // track renders the same bits on every thread.
        const juce::ScopedNoDenormals noDenormals;
        std::uint32_t seen = pool.wake.load(std::memory_order_acquire);
        while (!pool.stopping.load(std::memory_order_acquire))
        {
            {
                const rt::ScopedAudioThread audioThread; // tasks are audio work: the checks apply
                pool.work(thread);
            }
            // While jobs with work come quickly, stay awake for the next (it's due within one
            // interval; a quarter more, and the stream has stopped or slowed: sleep).
            bool woken = false;
            const auto interval = pool.recentInterval.load(std::memory_order_relaxed);
            if (pool.recentWork.load(std::memory_order_relaxed) >= wakeAfterNanoseconds && interval > 0
                && interval <= maxSpinNanoseconds)
            {
                const auto from = nowNanoseconds();
                for (int i = 0;; ++i)
                {
                    if (pool.wake.load(std::memory_order_acquire) != seen)
                    {
                        woken = true;
                        break;
                    }
                    if (i % 16 == 15 && nowNanoseconds() - from >= interval + interval / 4)
                        break;
                    juce::Thread::yield();
                }
            }
            if (!woken)
            {
                pool.sleepers.fetch_add(1, std::memory_order_acq_rel);
                if (pool.wake.load(std::memory_order_acquire) == seen && !pool.stopping.load(std::memory_order_acquire))
                    pool.wake.wait(seen, std::memory_order_acquire);
                pool.sleepers.fetch_sub(1, std::memory_order_acq_rel);
            }
            seen = pool.wake.load(std::memory_order_acquire);
        }
    }

private:
    WorkerPool& pool;
    const int thread;
};

WorkerPool::WorkerPool(int numWorkers) : busy(std::make_unique<Busy[]>(static_cast<std::size_t>(std::max(0, numWorkers)) + 1))
{
    for (int i = 0; i < numWorkers; ++i)
    {
        threads.push_back(std::make_unique<Worker>(*this, i));
        juce::Thread::RealtimeOptions options;
        if (!threads.back()->startRealtimeThread(options.withPriority(10)))
            threads.back()->startThread(juce::Thread::Priority::highest); // no real-time rights: still run
    }
}

WorkerPool::~WorkerPool()
{
    stopping.store(true, std::memory_order_release);
    wake.fetch_add(1, std::memory_order_acq_rel);
    wake.notify_all();
    for (auto& t : threads)
        t->stopThread(2000);
}

int WorkerPool::defaultWorkers()
{
    return std::max(0, juce::SystemStats::getNumPhysicalCpus() - 1);
}

std::vector<std::uint64_t> WorkerPool::busyNanoseconds() const
{
    std::vector<std::uint64_t> times;
    if (threads.empty())
        return times;
    for (std::size_t i = 0; i <= threads.size(); ++i)
        times.push_back(busy[i].nanoseconds.load(std::memory_order_relaxed));
    return times;
}

void WorkerPool::work(int thread) const noexcept
{
    // One clock read per task: each task's time runs from the end of the one before (the claims
    // between them take nanoseconds).
    auto from = nowNanoseconds();
    for (;;)
    {
        // The claim and the count it is checked against come from one atomic value: the job this
        // ticket was published for (whose fields the acquire makes visible). Between jobs the
        // count is 0, so a late claim finds nothing.
        const auto t = ticket.fetch_add(1, std::memory_order_acq_rel);
        const auto index = static_cast<std::uint32_t>(t & 0xffffffffu);
        const auto count = static_cast<std::uint32_t>(t >> 32);
        if (index >= count)
            return;
        jobTask(jobContext, static_cast<int>(index));
        const auto to = nowNanoseconds();
        busy[static_cast<std::size_t>(thread)].nanoseconds.fetch_add(to - from, std::memory_order_relaxed);
        from = to;
        done.fetch_add(1, std::memory_order_acq_rel);
    }
}

void WorkerPool::run(int count, Task task, void* context) const noexcept
{
    if (count <= 0)
        return;
    if (threads.empty())
    {
        for (int i = 0; i < count; ++i)
            task(context, i);
        return;
    }
    if (count == 1) // not worth waking anyone
    {
        const auto from = nowNanoseconds();
        task(context, 0);
        busy[0].nanoseconds.fetch_add(nowNanoseconds() - from, std::memory_order_relaxed);
        return;
    }

    // Publish the job: its fields first, then the ticket (its count, index 0). Workers still awake
    // (spinning after the last job) see the new generation and join at once.
    const auto start = nowNanoseconds();
    jobTask = task;
    jobContext = context;
    done.store(0, std::memory_order_relaxed);
    ticket.store(static_cast<std::uint64_t>(count) << 32, std::memory_order_release);
    wake.fetch_add(1, std::memory_order_acq_rel);

    // Sleeping workers are woken for a job with work in it: at once if the last one had, else once
    // this one has proved it. Only they need the kernel; the wake itself never waits.
    bool woken = false;
    const auto wakeSleepers = [this, &woken]() noexcept {
        woken = true;
        if (sleepers.load(std::memory_order_acquire) > 0)
        {
            LATTICE_RT_NONBLOCKING_SYSCALL;
            wake.notify_all();
        }
    };
    if (recentWork.load(std::memory_order_relaxed) >= wakeAfterNanoseconds)
        wakeSleepers();

    // Help (as work() does, checking the time between tasks), then wait for the stragglers (they
    // are mid-task: a short spin).
    auto from = start;
    for (;;)
    {
        const auto t = ticket.fetch_add(1, std::memory_order_acq_rel);
        const auto index = static_cast<std::uint32_t>(t & 0xffffffffu);
        if (index >= static_cast<std::uint32_t>(t >> 32))
            break;
        task(context, static_cast<int>(index));
        const auto to = nowNanoseconds();
        busy[0].nanoseconds.fetch_add(to - from, std::memory_order_relaxed);
        from = to;
        done.fetch_add(1, std::memory_order_acq_rel);
        if (!woken && to - start >= wakeAfterNanoseconds)
            wakeSleepers();
    }
    while (done.load(std::memory_order_acquire) < count)
        LATTICE_CPU_PAUSE(); // no system call: the audio thread never blocks

    // What the workers decide by: this job's task time on every thread (each thread's addition
    // came before its `done`, which the acquire above has seen), and the time since the last.
    std::uint64_t total = 0;
    for (std::size_t i = 0; i <= threads.size(); ++i)
        total += busy[i].nanoseconds.load(std::memory_order_relaxed);
    const auto decayed = [](std::uint64_t latest, std::uint64_t recent) { return std::max(latest, recent - recent / 16); };
    recentWork.store(decayed(total - busyTotal, recentWork.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    busyTotal = total;
    if (lastJobStart > 0)
        recentInterval.store(decayed(start - lastJobStart, recentInterval.load(std::memory_order_relaxed)), std::memory_order_relaxed);
    lastJobStart = start;
    // Every task is claimed, so the ticket's index is past its count: a worker that comes back
    // late finds nothing, even while the next job is being written.
}

} // namespace lattice
