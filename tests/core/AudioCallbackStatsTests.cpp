#include "core/AudioCallbackStats.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using Catch::Approx;
using lattice::AudioCallbackStats;

TEST_CASE("Callback stats are empty before any callback", "[core][stats]")
{
    AudioCallbackStats stats;
    const auto s = stats.summary();
    CHECK(s.callbacks == 0);
    CHECK(s.overruns == 0);
    CHECK(s.p50 == 0.0);
    CHECK(s.max == 0.0);
}

TEST_CASE("Callback stats percentiles follow synthetic loads", "[core][stats]")
{
    AudioCallbackStats stats;
    // 1000 callbacks: 900 at 10%, 90 at 40%, 9 at 65%, 1 at 150% (an overrun).
    for (int i = 0; i < 900; ++i)
        stats.recordLoad(0.10);
    for (int i = 0; i < 90; ++i)
        stats.recordLoad(0.40);
    for (int i = 0; i < 9; ++i)
        stats.recordLoad(0.65);
    stats.recordLoad(1.50);

    const auto s = stats.summary();
    const double bucket = 1.0 / AudioCallbackStats::bucketsPerPeriod;
    CHECK(s.callbacks == 1000);
    CHECK(s.overruns == 1);
    CHECK(s.p50 == Approx(0.10).margin(bucket));
    CHECK(s.p99 == Approx(0.40).margin(bucket));
    CHECK(s.p999 == Approx(0.65).margin(bucket));
    CHECK(s.max == Approx(1.50));
}

TEST_CASE("recordCallback divides by the buffer period", "[core][stats]")
{
    AudioCallbackStats stats;
    const double period = 64.0 / 48000.0;
    stats.recordCallback(period * 0.5, period);
    stats.recordCallback(period * 2.0, period);
    stats.recordCallback(1.0, 0.0); // ignored: no period
    const auto s = stats.summary();
    CHECK(s.callbacks == 2);
    CHECK(s.overruns == 1);
    CHECK(s.max == Approx(2.0));
}

TEST_CASE("Loads outside the histogram land in the edge buckets", "[core][stats]")
{
    AudioCallbackStats stats;
    stats.recordLoad(50.0);
    stats.recordLoad(-1.0);
    const auto s = stats.summary();
    CHECK(s.callbacks == 2);
    CHECK(s.overruns == 1);
    CHECK(s.max == Approx(50.0));
}

TEST_CASE("reset starts a new measurement window", "[core][stats]")
{
    AudioCallbackStats stats;
    for (int i = 0; i < 100; ++i)
        stats.recordLoad(0.9);
    stats.recordLoad(1.2);
    stats.reset();

    CHECK(stats.summary().callbacks == 0);
    CHECK(stats.summary().max == 0.0);

    stats.recordLoad(0.2);
    const auto s = stats.summary();
    CHECK(s.callbacks == 1);
    CHECK(s.overruns == 0);
    CHECK(s.max == Approx(0.2));
    CHECK(s.p999 <= 0.2);
}
