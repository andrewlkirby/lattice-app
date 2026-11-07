// LatticePluginScanner (docs/07-m6-plugins-external.md, 6a): describes plugins in a process of its
// own, so a plugin that crashes or hangs while it's loaded takes down only this.
//
//   LatticePluginScanner --jobs <file> --results <file> [--first <n>]
//
// Scans the jobs file's lines from the n-th (0 by default), appending to the results file (see
// ScanProtocol.h). Each job is one message on the message thread, so a plugin's own messages are
// handled in between, as in a host.

#include "hosting/PluginFormats.h"
#include "hosting/clap/ClapFormat.h"
#include "hosting/scan/ScanProtocol.h"

#include <csignal>
#include <cstdio>
#include <cstring>

#if JUCE_WINDOWS
#include <windows.h>

#include <crtdbg.h>
#include <io.h>
#else
#include <unistd.h>
#endif

using namespace lattice::hosting;

namespace
{
std::FILE* results = nullptr;

// What the crash handler writes: made before each job, since it can't allocate.
char crashText[64] = {};

void writeLine(const juce::String& line)
{
    const auto text = line + "\n";
    std::fputs(text.toRawUTF8(), results);
    std::fflush(results); // to the OS: what the app reads, even if this process dies next
}

#if JUCE_WINDOWS
LONG WINAPI onCrash(EXCEPTION_POINTERS* info)
{
    // Its own buffer: the handler mustn't depend on the heap the plugin may have damaged.
    char line[160];
    const auto code = static_cast<unsigned long>(info->ExceptionRecord->ExceptionCode);
    const char* name = code == EXCEPTION_ACCESS_VIOLATION     ? "access violation"
                       : code == EXCEPTION_STACK_OVERFLOW     ? "stack overflow"
                       : code == EXCEPTION_ILLEGAL_INSTRUCTION ? "illegal instruction"
                       : code == EXCEPTION_INT_DIVIDE_BY_ZERO ? "division by zero"
                                                              : "exception";
    std::snprintf(line, sizeof(line), "%s%s 0x%08lx\n", crashText, name, code);
    std::fputs(line, results);
    std::fflush(results);
    return EXCEPTION_EXECUTE_HANDLER; // end the process, with the exception's code
}

void onAbort(int)
{
    char line[96];
    std::snprintf(line, sizeof(line), "%sabort\n", crashText);
    std::fputs(line, results);
    std::fflush(results);
    TerminateProcess(GetCurrentProcess(), 3);
}

void installCrashHandlers()
{
    // No dialogs: Windows Error Reporting's, or the C runtime's for abort() and asserts, which would
    // leave the process waiting for a click until the app gives up on it.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    SetUnhandledExceptionFilter(onCrash);
    std::signal(SIGABRT, onAbort);
}
#else
void onSignal(int signal)
{
    // Only what's safe in a signal handler: write() of what was made before.
    char line[96];
    const auto prefix = std::strlen(crashText);
    std::memcpy(line, crashText, prefix);
    const char* name = signal == SIGSEGV ? "segmentation fault" : signal == SIGBUS ? "bus error"
                       : signal == SIGILL ? "illegal instruction" : signal == SIGFPE ? "arithmetic error"
                       : signal == SIGABRT ? "abort" : "signal";
    const auto n = std::strlen(name);
    std::memcpy(line + prefix, name, n);
    line[prefix + n] = '\n';
    [[maybe_unused]] const auto written = ::write(fileno(results), line, prefix + n + 1);
    std::signal(signal, SIG_DFL);
    std::raise(signal);
}

void installCrashHandlers()
{
    for (const int s : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT})
        std::signal(s, onSignal);
}
#endif

std::FILE* openForAppending(const juce::File& file)
{
#if JUCE_WINDOWS
    return _wfsopen(file.getFullPathName().toWideCharPointer(), L"ab", _SH_DENYNO);
#else
    return std::fopen(file.getFullPathName().toRawUTF8(), "ab");
#endif
}

class Scanner final
{
public:
    Scanner(std::vector<scan::Job> j, int first) : jobs(std::move(j)), next(first) { addHostedFormats(formats); }

    void start() { post(); }

private:
    void post()
    {
        juce::MessageManager::callAsync([this] { scanNext(); });
    }

    void scanNext()
    {
        if (next >= static_cast<int>(jobs.size()))
        {
            writeLine(scan::doneLine());
            juce::MessageManager::getInstance()->stopDispatchLoop();
            return;
        }
        const int n = next++;
        const auto prefix = scan::crashLine(n, {}).toStdString();
        std::snprintf(crashText, sizeof(crashText), "%s", prefix.c_str());
        writeLine(scan::beginLine(n));
        scanJob(n, jobs[static_cast<std::size_t>(n)]);
        writeLine(scan::endLine(n));
        post();
    }

    void scanJob(int n, const scan::Job& job)
    {
        if (job.format == ClapPluginFormat::formatName)
        {
            juce::String error;
            const auto plugins = ClapPluginFormat::scanFile(job.identifier, error);
            for (const auto& p : plugins)
                writeLine(scan::pluginLine(n, p));
            if (plugins.empty() && error.isNotEmpty())
                writeLine(scan::failLine(n, error));
            return;
        }
        juce::AudioPluginFormat* format = nullptr;
        for (auto* f : formats.getFormats())
            if (f->getName() == job.format)
                format = f;
        if (format == nullptr)
        {
            writeLine(scan::failLine(n, "this build of Lattice doesn't host " + job.format));
            return;
        }
        juce::OwnedArray<juce::PluginDescription> found;
        format->findAllTypesForFile(found, job.identifier);
        for (const auto* d : found)
        {
            scan::ScannedPlugin p;
            p.description = *d;
            writeLine(scan::pluginLine(n, p));
        }
    }

    juce::AudioPluginFormatManager formats;
    std::vector<scan::Job> jobs;
    int next = 0;
};
} // namespace

int main(int argc, char* argv[])
{
    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add(juce::CharPointer_UTF8(argv[i]));
    const auto value = [&](const char* name) {
        const int i = args.indexOf(name);
        return i >= 0 && i + 1 < args.size() ? args[i + 1] : juce::String();
    };
    const juce::File jobsFile(value("--jobs")), resultsFile(value("--results"));
    if (!jobsFile.existsAsFile() || value("--results").isEmpty())
    {
        std::fputs("usage: LatticePluginScanner --jobs <file> --results <file> [--first <n>]\n", stderr);
        return 2;
    }
    results = openForAppending(resultsFile);
    if (results == nullptr)
        return 2;
    installCrashHandlers();

    const juce::ScopedJuceInitialiser_GUI juceInit; // plugins expect a message thread, as in a host
    Scanner scanner(scan::readJobs(jobsFile.loadFileAsString()), juce::jmax(0, value("--first").getIntValue()));
    scanner.start();
    juce::MessageManager::getInstance()->runDispatchLoop();
    std::fclose(results);
    return 0;
}
