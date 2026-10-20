#pragma once

#include <juce_events/juce_events.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace lattice
{

/** Background work for the UI and the library (docs/05-m4-ui.md, "Rules", 4; charter §4: no disk
    I/O or long work on the message thread): loading, indexing, building waveform peaks, saving.

    Jobs run on worker threads at normal priority, never on the audio or real-time worker threads.
    Each job can report its progress and check whether it was cancelled; when it ends, its `done`
    callback runs on the message thread. The running jobs (with names and progress) are listed for
    the status bar. Destroying the pool cancels what's left and waits for running jobs to stop. */
class JobPool final : private juce::AsyncUpdater
{
public:
    /** What a running job sees. Any thread (the job's). */
    class Context
    {
    public:
        virtual ~Context() = default;
        /** 0..1, or a negative value for "unknown". */
        virtual void setProgress(float progress) = 0;
        virtual bool isCancelled() const = 0;
    };

    using Work = std::function<void(Context&)>;
    /** cancelled: the job was cancelled (before or while it ran). Message thread. */
    using Done = std::function<void(bool cancelled)>;

    struct Info
    {
        std::uint64_t id = 0;
        juce::String name;
        float progress = -1.0f; // negative: unknown
        bool running = false;   // false: waiting for a thread
    };

    /** threads: 0 = half the CPU's cores (at least one), leaving the rest for audio. */
    explicit JobPool(int threads = 0);
    ~JobPool() override;

    int threadCount() const noexcept { return numThreads; }

    /** Message thread. Returns the job's ID (for cancel()). */
    std::uint64_t submit(juce::String name, Work work, Done done = {});
    /** Message thread. A waiting job never runs; a running one sees isCancelled(). Its done callback
        still runs (with cancelled = true). */
    void cancel(std::uint64_t id);
    void cancelAll();

    /** The jobs waiting or running now. Any thread. */
    std::vector<Info> jobs() const;
    bool idle() const;

    /** Called on the message thread when the list of jobs or their progress changed (at most once
        per message-loop turn). */
    std::function<void()> onJobsChanged;

    /** Runs the done callbacks of finished jobs now (normally the message loop does this; tests
        call it). Message thread. Returns how many ran. */
    int deliverFinished();
    /** For tests: waits up to `timeoutMs` for every job to finish running. Message thread. */
    bool waitUntilIdle(int timeoutMs);

private:
    struct Job;
    class ThreadJob;
    void handleAsyncUpdate() override;
    void finished(const std::shared_ptr<Job>& job);
    void changed();

    int numThreads;
    std::unique_ptr<juce::ThreadPool> pool;
    mutable std::mutex lock;
    std::vector<std::shared_ptr<Job>> active;   // waiting or running
    std::vector<std::shared_ptr<Job>> finishedJobs; // waiting for their done callback
    std::uint64_t nextId = 1;
    std::atomic<bool> listChanged{false};
};

} // namespace lattice
