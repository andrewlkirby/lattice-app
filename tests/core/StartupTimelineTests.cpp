#include "core/StartupTimeline.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using lattice::StartupTimeline;

TEST_CASE("Startup timeline records stages in order with their times", "[core][startup]")
{
    double now = 0.05;
    StartupTimeline timeline([&] { return now; });

    timeline.mark("app init");
    now = 0.31;
    timeline.mark("window shown");
    now = 0.62;
    timeline.mark("audio started");
    now = 0.63;
    timeline.mark("ready");

    const auto entries = timeline.entries();
    REQUIRE(entries.size() == 4);
    CHECK(entries[0].stage == "app init");
    CHECK(entries[1].stage == "window shown");
    CHECK(entries[2].stage == "audio started");
    CHECK(entries[3].stage == "ready");
    for (std::size_t i = 1; i < entries.size(); ++i)
        CHECK(entries[i].seconds >= entries[i - 1].seconds);
    CHECK(*timeline.timeOf("window shown") == Catch::Approx(0.31));
    CHECK_FALSE(timeline.timeOf("library ready").has_value());
}

TEST_CASE("Marking a stage twice keeps the first time", "[core][startup]")
{
    double now = 1.0;
    StartupTimeline timeline([&] { return now; });
    timeline.mark("ready");
    now = 2.0;
    CHECK(timeline.mark("ready") == Catch::Approx(1.0));
    CHECK(timeline.entries().size() == 1);
}

TEST_CASE("Startup log line lists every stage in seconds", "[core][startup]")
{
    double now = 0.1234;
    StartupTimeline timeline([&] { return now; });
    timeline.mark("window shown");
    now = 1.5;
    timeline.mark("ready");

    const auto line = timeline.toLogLine(juce::Time(2026, 9, 11, 12, 0, 0));
    CHECK(line.contains(" | window shown=0.123 | ready=1.500"));
}

TEST_CASE("Startup log is appended to and trimmed", "[core][startup]")
{
    const auto dir = juce::File::createTempFile("lattice-startup-test");
    const auto log = dir.getChildFile("logs").getChildFile("startup.log");

    StartupTimeline timeline([] { return 0.2; });
    timeline.mark("ready");
    constexpr int maxLines = 10;
    for (int i = 0; i < maxLines + 5; ++i)
        REQUIRE(timeline.appendTo(log, maxLines));

    juce::StringArray lines;
    lines.addLines(log.loadFileAsString());
    lines.removeEmptyStrings();
    CHECK(lines.size() == maxLines);
    CHECK(lines[lines.size() - 1].contains("ready=0.200"));

    dir.deleteRecursively();
}

TEST_CASE("The default clock measures from process start", "[core][startup]")
{
    StartupTimeline timeline;
    const double t = timeline.mark("now");
    CHECK(t > 0.0);
    CHECK(t < 3600.0);
}
