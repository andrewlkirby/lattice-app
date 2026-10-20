#include "engine/WorkerPool.h"

#include "platform/Platform.h"

#include <juce_audio_basics/juce_audio_basics.h>

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

class WorkerPool::Worker final : public juce::Thread
{
public:
    Worker(WorkerPool& p, int index) : juce::Thread("Lattice audio worker " + juce::String(index + 1)), pool(p) {}

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
                pool.work();
            }
            // Spin a little: the next block's job usually comes soon.
            bool woken = false;
            for (int i = 0; i < spinIterations; ++i)
            {
                if (pool.wake.load(std::memory_order_acquire) != seen)
                {
                    woken = true;
                    break;
                }
                juce::Thread::yield();
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
};

WorkerPool::WorkerPool(int numWorkers)
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

void WorkerPool::work() const noexcept
{
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
        done.fetch_add(1, std::memory_order_acq_rel);
    }
}

void WorkerPool::run(int count, Task task, void* context) const noexcept
{
    if (count <= 0)
        return;
    if (threads.empty() || count == 1)
    {
        for (int i = 0; i < count; ++i)
            task(context, i);
        return;
    }

    // Publish the job: its fields first, then the ticket (its count, index 0).
    jobTask = task;
    jobContext = context;
    done.store(0, std::memory_order_relaxed);
    ticket.store(static_cast<std::uint64_t>(count) << 32, std::memory_order_release);
    wake.fetch_add(1, std::memory_order_acq_rel);
    if (sleepers.load(std::memory_order_acquire) > 0)
    {
        // Only workers that went to sleep (no job for a while) need the kernel; the wake itself
        // never waits.
        LATTICE_RT_NONBLOCKING_SYSCALL;
        wake.notify_all();
    }

    // Help, then wait for the stragglers (they are mid-task: a short spin).
    work();
    while (done.load(std::memory_order_acquire) < count)
        LATTICE_CPU_PAUSE(); // no system call: the audio thread never blocks
    // Every task is claimed, so the ticket's index is past its count: a worker that comes back
    // late finds nothing, even while the next job is being written.
}

} // namespace lattice
