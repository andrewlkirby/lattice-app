#include "hosting/scan/PluginScanner.h"

#include <cmath>

namespace lattice::hosting::scan
{

namespace
{
/** Reads what's been added to the results file since last time, a whole line at a time. */
class ResultsReader
{
public:
    explicit ResultsReader(juce::File f) : file(std::move(f)) {}

    template <typename Handle>
    void read(Handle&& handle)
    {
        juce::FileInputStream in(file);
        if (!in.openedOk() || in.getTotalLength() <= offset || !in.setPosition(offset))
            return;
        const auto available = static_cast<std::size_t>(in.getTotalLength() - offset);
        const auto start = pending.getSize();
        pending.setSize(start + available);
        const auto got = in.read(static_cast<char*>(pending.getData()) + start, static_cast<int>(available));
        pending.setSize(start + static_cast<std::size_t>(juce::jmax(0, got)));
        offset += juce::jmax(0, got);
        // Lines are split on bytes, then decoded: a read may end inside a character.
        const auto* bytes = static_cast<const char*>(pending.getData());
        std::size_t lineStart = 0;
        for (std::size_t i = 0; i < pending.getSize(); ++i)
            if (bytes[i] == '\n')
            {
                const auto line = juce::String::fromUTF8(bytes + lineStart, static_cast<int>(i - lineStart)).trimCharactersAtEnd("\r");
                if (const auto parsed = parseLine(line))
                    handle(*parsed);
                lineStart = i + 1;
            }
        if (lineStart > 0)
            pending.removeSection(0, lineStart);
    }

private:
    juce::File file;
    juce::int64 offset = 0;
    juce::MemoryBlock pending;
};
} // namespace

juce::String PluginScanner::describe(Outcome outcome)
{
    switch (outcome)
    {
    case Outcome::found: return "found";
    case Outcome::noPlugins: return "no plugins";
    case Outcome::failed: return "failed";
    case Outcome::crashed: return "crashed";
    case Outcome::hung: return "hung";
    }
    return {};
}

juce::String PluginScanner::exitDescription(juce::uint32 code)
{
#if JUCE_WINDOWS
    switch (code)
    {
    case 0xC0000005u: return "access violation";
    case 0xC00000FDu: return "stack overflow";
    case 0xC000001Du: return "illegal instruction";
    case 0xC0000094u: return "division by zero";
    case 0xC0000409u: return "stack buffer overrun";
    case 0xC0000374u: return "heap corruption";
    case 0xC0000135u: return "a DLL it needs is missing";
    case 3u: return "abort";
    default: break;
    }
    if ((code & 0xC0000000u) == 0xC0000000u)
        return "exception 0x" + juce::String::toHexString(static_cast<int>(code)).paddedLeft('0', 8);
#endif
    return "exit code " + juce::String(code);
}

PluginScanner::PluginScanner(Options o) : options(std::move(o)) {}

bool PluginScanner::run(const std::vector<Job>& jobs, const std::function<void(const Result&)>& onResult,
                        const std::function<bool()>& cancelled)
{
    starts = 0;
    lastError.clear();
    if (jobs.empty())
        return true;
    if (!options.scanner.existsAsFile())
    {
        lastError = "The plugin scanner is missing: " + options.scanner.getFullPathName();
        return false;
    }
    const auto folder = options.workFolder.getChildFile("scan-" + juce::Uuid().toString());
    if (!folder.createDirectory())
    {
        lastError = "Couldn't make a folder for the plugin scan: " + folder.getFullPathName();
        return false;
    }
    const juce::ScopeGuard cleanUp{[&] { folder.deleteRecursively(); }};
    const auto jobsFile = folder.getChildFile("jobs.txt");
    const auto resultsFile = folder.getChildFile("results.txt");
    if (!jobsFile.replaceWithText(writeJobs(jobs)) || !resultsFile.create())
    {
        lastError = "Couldn't write the plugin scan's files in " + folder.getFullPathName();
        return false;
    }

    ResultsReader reader(resultsFile);
    const int count = static_cast<int>(jobs.size());
    const auto limitMs = options.timeLimitSeconds * 1000.0;
    int next = 0;
    int startsWithoutProgress = 0;
    while (next < count)
    {
        if (cancelled && cancelled())
            return false;
        juce::ChildProcess process;
        const juce::StringArray args{options.scanner.getFullPathName(), "--jobs", jobsFile.getFullPathName(),
                                     "--results", resultsFile.getFullPathName(), "--first", juce::String(next)};
        if (!process.start(args, 0))
        {
            lastError = "The plugin scanner couldn't be started: " + options.scanner.getFullPathName();
            return false;
        }
        ++starts;

        int current = -1;
        double began = 0.0;
        Result building;
        juce::String crash;
        bool done = false, progressed = false;
        const auto finish = [&](Outcome outcome, const juce::String& reason) {
            building.outcome = outcome;
            if (reason.isNotEmpty())
                building.reason = reason;
            if (outcome == Outcome::crashed || outcome == Outcome::hung)
                building.plugins.clear(); // what it said before going down isn't to be trusted
            if (onResult)
                onResult(building);
            next = current + 1;
            current = -1;
        };
        const auto handle = [&](const Line& line) {
            switch (line.type)
            {
            case Line::Type::begin:
                if (line.job < 0 || line.job >= count)
                    return;
                current = line.job;
                began = juce::Time::getMillisecondCounterHiRes();
                building = {};
                building.job = jobs[static_cast<std::size_t>(current)];
                crash.clear();
                progressed = true;
                break;
            case Line::Type::plugin:
                if (line.job == current && line.plugin)
                    building.plugins.push_back(*line.plugin);
                break;
            case Line::Type::fail:
                if (line.job == current)
                    building.reason = line.text;
                break;
            case Line::Type::crash:
                if (line.job == current)
                    crash = line.text;
                break;
            case Line::Type::end:
                if (line.job == current)
                    finish(!building.plugins.empty() ? Outcome::found
                                                     : building.reason.isNotEmpty() ? Outcome::failed : Outcome::noPlugins,
                           {});
                break;
            case Line::Type::done: done = true; break;
            }
        };

        for (;;)
        {
            // Whether it's still running first, then what it wrote: so nothing written before
            // it ended is missed.
            const bool running = process.isRunning();
            reader.read(handle);
            if (done)
                break;
            if (cancelled && cancelled())
            {
                process.kill();
                return false;
            }
            if (current >= 0 && juce::Time::getMillisecondCounterHiRes() - began > limitMs)
            {
                process.kill();
                process.waitForProcessToFinish(5000);
                const auto limit = options.timeLimitSeconds;
                finish(Outcome::hung, "took longer than " + juce::String(limit, limit == std::floor(limit) ? 0 : 1) + " s");
                break;
            }
            if (!running)
            {
                if (current >= 0)
                    finish(Outcome::crashed, crash.isNotEmpty() ? crash : exitDescription(process.getExitCode()));
                break;
            }
            juce::Thread::sleep(options.pollMilliseconds);
        }
        if (done)
            break;
        if (progressed)
            startsWithoutProgress = 0;
        else if (++startsWithoutProgress >= 2)
        {
            lastError = "The plugin scanner stopped before scanning anything (" + exitDescription(process.getExitCode()) + ")";
            return false;
        }
    }
    return true;
}

} // namespace lattice::hosting::scan
