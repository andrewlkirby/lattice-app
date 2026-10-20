// The background job pool (docs/05-m4-ui.md, "Rules", 4): work runs off the message thread, its
// result comes back on the message thread, progress is visible, and jobs can be cancelled whether
// waiting or running, including by destroying the pool.

#include "core/JobPool.h"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <thread>

using namespace lattice;

TEST_CASE("Jobs: work runs on a worker thread and reports back on the message thread", "[core][jobs]")
{
    JobPool pool(2);
    CHECK(pool.threadCount() == 2);
    CHECK(JobPool().threadCount() >= 1);

    std::atomic<int> ran{0};
    std::thread::id workThread;
    int doneCalls = 0;
    bool wasCancelled = true;
    pool.submit("Count", [&](JobPool::Context& c) {
        workThread = std::this_thread::get_id();
        c.setProgress(0.5f);
        ++ran;
    }, [&](bool cancelled) {
        ++doneCalls;
        wasCancelled = cancelled;
        CHECK(juce::MessageManager::getInstance()->isThisTheMessageThread());
    });
    REQUIRE(pool.waitUntilIdle(5000));
    CHECK(ran == 1);
    CHECK(workThread != std::this_thread::get_id());
    CHECK(doneCalls == 0); // not until the message thread delivers it
    CHECK(pool.deliverFinished() == 1);
    CHECK(doneCalls == 1);
    CHECK(!wasCancelled);
    CHECK(pool.jobs().empty());
}

TEST_CASE("Jobs: the status bar sees each job's name and progress while it runs", "[core][jobs]")
{
    JobPool pool(1);
    std::atomic<bool> release{false}, started{false};
    pool.submit("Index library", [&](JobPool::Context& c) {
        c.setProgress(0.25f);
        started = true;
        while (!release)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    });
    pool.submit("Build peaks", [](JobPool::Context&) {});
    while (!started)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    const auto list = pool.jobs();
    REQUIRE(list.size() == 2);
    CHECK(list[0].name == "Index library");
    CHECK(list[0].running);
    CHECK(list[0].progress == 0.25f);
    CHECK(list[1].name == "Build peaks");
    CHECK(!list[1].running); // waiting for the one thread
    CHECK(list[1].progress < 0.0f); // unknown
    release = true;
    REQUIRE(pool.waitUntilIdle(5000));
    CHECK(pool.idle());
}

TEST_CASE("Jobs: a cancelled job never runs if it was waiting, and stops if it was running", "[core][jobs]")
{
    JobPool pool(1);
    std::atomic<bool> runningStarted{false};
    std::atomic<int> loops{0};
    bool firstCancelled = false, secondCancelled = false, secondRan = false;
    const auto first = pool.submit("Loop", [&](JobPool::Context& c) {
        runningStarted = true;
        while (!c.isCancelled())
        {
            ++loops;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }, [&](bool cancelled) { firstCancelled = cancelled; });
    const auto second = pool.submit("Waiting", [&](JobPool::Context&) { secondRan = true; },
                                    [&](bool cancelled) { secondCancelled = cancelled; });
    while (!runningStarted)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));

    pool.cancel(second);
    pool.cancel(first);
    REQUIRE(pool.waitUntilIdle(5000));
    CHECK(pool.deliverFinished() == 2);
    CHECK(firstCancelled);
    CHECK(secondCancelled);
    CHECK(!secondRan);
    CHECK(loops > 0);
}

TEST_CASE("Jobs: destroying the pool stops its running jobs", "[core][jobs]")
{
    std::atomic<bool> stopped{false};
    std::atomic<bool> started{false};
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    {
        JobPool pool(1);
        pool.submit("Forever", [&](JobPool::Context& c) {
            started = true;
            while (!c.isCancelled())
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            stopped = true;
        });
        while (!started)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(stopped);
    CHECK(juce::Time::getMillisecondCounterHiRes() - t0 < 2000.0);
}
