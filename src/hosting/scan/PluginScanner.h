#pragma once

#include "hosting/scan/ScanProtocol.h"

#include <functional>
#include <vector>

namespace lattice::hosting::scan
{

/** Runs LatticePluginScanner over a list of jobs (docs/07-m6-plugins-external.md, 6a), and says
    how each one went. A job the scanner crashes on, or takes longer than the time limit over, is
    given that outcome, and the scanner starts again from the next one: one bad plugin costs one
    restart, never the scan, and never the app.

    run() blocks; it's for a background thread. */
class PluginScanner
{
public:
    struct Options
    {
        juce::File scanner;              // the LatticePluginScanner executable
        juce::File workFolder;           // where the jobs and results files go (created; emptied after)
        double timeLimitSeconds = 30.0;  // for each job
        int pollMilliseconds = 10;
    };

    enum class Outcome
    {
        found,     // plugins found
        noPlugins, // the file holds none this format can load
        failed,    // the format couldn't read it (reason says why)
        crashed,   // the scanner crashed on it (reason says how)
        hung,      // it took longer than the time limit
    };
    static juce::String describe(Outcome outcome);

    struct Result
    {
        Job job;
        Outcome outcome = Outcome::noPlugins;
        juce::String reason;
        std::vector<ScannedPlugin> plugins;
    };

    explicit PluginScanner(Options options);

    /** Scans the jobs in order, calling `onResult` on this thread as each one finishes. False if
        it was cancelled, or the scanner can't run (then the jobs left are given no outcome). */
    bool run(const std::vector<Job>& jobs, const std::function<void(const Result&)>& onResult,
             const std::function<bool()>& cancelled = {});

    /** Why the last run() stopped early, if it did. */
    const juce::String& error() const noexcept { return lastError; }

    /** How many times the last run() started the scanner. */
    int processesStarted() const noexcept { return starts; }

    /** Windows: what an exception code means ("access violation"); elsewhere the exit code. */
    static juce::String exitDescription(juce::uint32 code);

private:
    Options options;
    juce::String lastError;
    int starts = 0;
};

} // namespace lattice::hosting::scan
