#include "core/JobPool.h"

#include <algorithm>

namespace lattice
{

struct JobPool::Job final : JobPool::Context
{
    std::uint64_t id = 0;
    juce::String name;
    Work work;
    Done done;
    JobPool* owner = nullptr;
    std::atomic<float> progress{-1.0f};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> running{false};

    void setProgress(float p) override
    {
        const float clamped = p < 0.0f ? -1.0f : juce::jmin(1.0f, p);
        if (progress.exchange(clamped) != clamped)
            owner->changed();
    }
    bool isCancelled() const override { return cancelled.load(); }
};

/** One job on a juce::ThreadPool thread. */
class JobPool::ThreadJob final : public juce::ThreadPoolJob
{
public:
    ThreadJob(JobPool& p, std::shared_ptr<Job> j) : juce::ThreadPoolJob(j->name), pool(p), job(std::move(j)) {}

    JobStatus runJob() override
    {
        if (!job->cancelled.load())
        {
            job->running = true;
            pool.changed();
            try
            {
                job->work(*job);
            }
            catch (...)
            {
                jassertfalse; // a job must not throw; treat it as finished
            }
        }
        pool.finished(job);
        return jobHasFinished;
    }

private:
    JobPool& pool;
    std::shared_ptr<Job> job;
};

JobPool::JobPool(int threads)
    : numThreads(threads > 0 ? threads : juce::jmax(1, juce::SystemStats::getNumPhysicalCpus() / 2))
{
    pool = std::make_unique<juce::ThreadPool>(
        juce::ThreadPoolOptions{}.withThreadName("Lattice jobs").withNumberOfThreads(numThreads));
}

JobPool::~JobPool()
{
    cancelAll();
    pool->removeAllJobs(true, 10000); // running jobs see isCancelled() and stop
    pool.reset();
    cancelPendingUpdate();
}

std::uint64_t JobPool::submit(juce::String name, Work work, Done done)
{
    auto job = std::make_shared<Job>();
    job->name = std::move(name);
    job->work = std::move(work);
    job->done = std::move(done);
    job->owner = this;
    {
        const std::scoped_lock guard(lock);
        job->id = nextId++;
        active.push_back(job);
    }
    changed();
    pool->addJob(new ThreadJob(*this, job), true);
    return job->id;
}

void JobPool::cancel(std::uint64_t id)
{
    const std::scoped_lock guard(lock);
    for (auto& j : active)
        if (j->id == id)
            j->cancelled = true;
}

void JobPool::cancelAll()
{
    const std::scoped_lock guard(lock);
    for (auto& j : active)
        j->cancelled = true;
}

std::vector<JobPool::Info> JobPool::jobs() const
{
    const std::scoped_lock guard(lock);
    std::vector<Info> out;
    out.reserve(active.size());
    for (const auto& j : active)
        out.push_back({j->id, j->name, j->progress.load(), j->running.load()});
    return out;
}

bool JobPool::idle() const
{
    const std::scoped_lock guard(lock);
    return active.empty();
}

void JobPool::finished(const std::shared_ptr<Job>& job)
{
    {
        const std::scoped_lock guard(lock);
        std::erase(active, job);
        finishedJobs.push_back(job);
    }
    changed();
}

void JobPool::changed()
{
    listChanged = true;
    triggerAsyncUpdate();
}

int JobPool::deliverFinished()
{
    std::vector<std::shared_ptr<Job>> ready;
    {
        const std::scoped_lock guard(lock);
        ready.swap(finishedJobs);
    }
    for (const auto& j : ready)
        if (j->done)
            j->done(j->cancelled.load());
    return static_cast<int>(ready.size());
}

bool JobPool::waitUntilIdle(int timeoutMs)
{
    const auto end = juce::Time::getMillisecondCounter() + static_cast<juce::uint32>(timeoutMs);
    while (!idle())
    {
        if (juce::Time::getMillisecondCounter() > end)
            return false;
        juce::Thread::sleep(1);
    }
    return true;
}

void JobPool::handleAsyncUpdate()
{
    deliverFinished();
    if (listChanged.exchange(false) && onJobsChanged)
        onJobsChanged();
}

} // namespace lattice
