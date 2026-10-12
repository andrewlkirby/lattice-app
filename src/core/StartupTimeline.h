#pragma once

#include <juce_core/juce_core.h>

#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace lattice
{

/** Timestamps each startup stage, measured from the moment the OS created the process
    (docs/09-performance-charter.md §1-2: window shown <= 0.5 s, ready <= 1.5 s).

    Stages may be marked from any thread except the audio thread. Each launch appends one line to
    <userdata>/logs/startup.log, which scripts/perf-check.ps1 reads. */
class StartupTimeline
{
public:
    struct Entry
    {
        juce::String stage;
        double seconds = 0.0; // since process start
    };

    /** clock returns seconds since process start. The default uses the OS process start time. */
    explicit StartupTimeline(std::function<double()> clock = {});

    /** Records a stage and returns its time. Marking the same stage again is ignored. */
    double mark(const juce::String& stage);

    std::vector<Entry> entries() const;
    std::optional<double> timeOf(const juce::String& stage) const;

    /** One line: "<ISO time> | stage=0.123 | stage=0.456 ...", times in seconds. */
    juce::String toLogLine(const juce::Time& now = juce::Time::getCurrentTime()) const;

    static constexpr int defaultMaxLogLines = 500;

    /** Appends toLogLine() to the file (creating folders as needed), keeping only the newest
        maxLines lines so the log stays small. Returns false if the file could not be written. */
    bool appendTo(const juce::File& logFile, int maxLines = defaultMaxLogLines) const;

private:
    std::function<double()> clock;
    mutable std::mutex mutex;
    std::vector<Entry> stages;
};

} // namespace lattice
