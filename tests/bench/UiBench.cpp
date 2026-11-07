// UI benchmarks (docs/05-m4-ui.md, Tests 10; docs/09-performance-charter.md "UI responsiveness"):
// the views that take the most drawing, frame by frame as the window would draw them, against the
// charter's 8 ms frame time while busy.
//
//   lattice_ui_bench [--frames N] [--filter name]
//   lattice_ui_bench --idle SECONDS [--audio [--device-type TYPE]] [--virtual-push]
//
// Each case paints its view headless (JUCE's software renderer into an image: the app's own
// renderer on Windows is Direct2D, usually faster) once per frame while the view changes, and
// reports the median, the 95th percentile and the worst frame. What isn't the UI's work (the
// engine rendering the audio the meters show, a test's copy of its rows) happens between frames,
// untimed. A case is over budget when its 95th
// percentile is above 8 ms. The run fails (exit code 1) if any case is over. scripts/linux-test.sh
// --callgrind counts the instructions in runFrames() against docs/perf/baselines/ui-callgrind.txt.
//
// --idle measures idle CPU (charter Â§1: with the transport stopped and nothing moving, at most
// 0.3% of one core above a bare JUCE window): the window on screen over a set with clips, its
// refresh running as in the app, nothing touched. After a 3 s settle it reports the process's CPU
// time and the message thread's over SECONDS, as shares of one core; then the same for a bare JUCE
// window of the same size (JUCE's own floor: its vblank thread, the window's message handling),
// and fails if the difference is over the budget. --audio also opens the audio device the app
// uses (its saved setup) and the app's worker threads, as when the app sits open with the
// transport stopped, and only reports (the driver's share of the audio thread isn't the app's);
// --device-type opens the same device through another type (e.g. "Windows Audio", which doesn't
// take the device from other programs as exclusive mode does). --virtual-push also shows Virtual Push
// docked in the window, with its controller following the set (docs/06-m5-controllers.md, 5a: shown,
// it stays within the same budget).

#include "devices/racks/Rack.h"
#include "engine/WorkerPool.h"
#include "platform/Platform.h"
#include "devices/racks/RackEditing.h"
#include "support/OfflineRenderer.h"
#include "support/TestDevices.h"
#include "ui/shell/MainShell.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>
#include <map>
#else
#include <ctime>
#endif

using namespace lattice;
using namespace lattice::ui;
using namespace lattice::test;

namespace
{
constexpr double budgetMs = 8.0;

/** A shell over a set, as the app opens it (with the browser and detail view shown). */
struct Window
{
    Window(int tracks, int scenes)
    {
        settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lattice-ui-bench-" + juce::Uuid().toString() + ".settings");
        juce::PropertiesFile::Options options;
        options.applicationName = "LatticeBench";
        settings = std::make_unique<juce::PropertiesFile>(settingsFile, options);
        session.newSet(tracks, scenes);
        session.flush();
        shell = std::make_unique<MainShell>(session, engine, *settings, tunings, ShellActions{});
        shell->setSize(1920, 1080);
        shell->setDetailVisible(true);
        settle();
    }
    ~Window()
    {
        shell.reset();
        settings.reset();
        settingsFile.deleteFile();
    }
    LiveSet& set() { return session.set(); }
    void settle()
    {
        session.flush();
        shell->sessionView().flushUpdates();
        shell->deviceView().flushUpdates();
        shell->clipView().flushUpdates();
    }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
    juce::File settingsFile;
    std::unique_ptr<juce::PropertiesFile> settings;
    std::unique_ptr<MainShell> shell;
};

/** One frame: what happens before it (untimed), then the UI's work (timed: the view changes,
    then paints). */
struct Frame
{
    std::function<void(int frame)> before, draw;
};

struct Case
{
    std::string name;
    /** Sets the case up and returns its frames. */
    std::function<Frame(Window&)> make;
    int tracks = 2, scenes = 2;
};

struct Result
{
    double median = 0.0, p95 = 0.0, worst = 0.0;
};

void paint(juce::Component& c, juce::Image& target)
{
    juce::Graphics g(target);
    c.paintEntireComponent(g, true);
}

#if defined(__GNUC__) || defined(__clang__)
#define LATTICE_BENCH_NOINLINE __attribute__((noinline))
#else
#define LATTICE_BENCH_NOINLINE __declspec(noinline)
#endif

/** The frames measured (callgrind counts what's in here). */
LATTICE_BENCH_NOINLINE std::vector<double> runFrames(const Frame& frame, int frames)
{
    std::vector<double> ms;
    ms.reserve(static_cast<std::size_t>(frames));
    for (int i = 0; i < frames; ++i)
    {
        if (frame.before)
            frame.before(i);
        const auto start = juce::Time::getHighResolutionTicks();
        frame.draw(i);
        ms.push_back(juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - start) * 1000.0);
    }
    return ms;
}

std::vector<Case> cases()
{
    std::vector<Case> list;

    // A 32 x 16 Session grid, every slot holding a clip, every track playing (its meters moving).
    list.push_back({"Session grid 32x16, meters moving",
                    [](Window& w) -> Frame {
                        auto& set = w.set();
                        for (int t = 0; t < set.numTracks(); ++t)
                        {
                            set.insertDevice(LiveSet::chainOf(set.track(t)), -1, TestSineDevice::typeId);
                            for (int s = 0; s < set.numScenes(); ++s)
                                set.createClip(t, s, 4.0);
                        }
                        w.settle();
                        auto renderer = std::make_shared<OfflineRenderer>(w.engine, BlockPattern::Fixed64);
                        renderer->attach(w.session);
                        w.session.launchScene(0);
                        w.session.play();
                        // The whole Session view (grid and mixer) each frame: more than the window
                        // repaints (only the meters and the playing slots), as a resize would.
                        auto& view = w.shell->sessionView();
                        auto target = std::make_shared<juce::Image>(juce::Image::ARGB, view.getWidth(), view.getHeight(), true);
                        return {[renderer](int) { renderer->renderFor(800); }, // a 60th of a second at 48 kHz
                                [&w, &view, target](int frame) {
                                    w.shell->feed().tick(frame / 60.0);
                                    paint(view, *target);
                                }};
                    },
                    32, 16});

    // The piano roll with 10,000 notes, zooming and scrolling every frame.
    list.push_back({"Piano roll 10000 notes, zoom and scroll",
                    [](Window& w) -> Frame {
                        auto& set = w.set();
                        auto clip = set.createClip(0, 0, 64.0 * 4.0);
                        juce::Random random(7);
                        w.session.undoManager().beginNewTransaction("Notes");
                        for (int i = 0; i < 10000; ++i)
                            set.addNote(clip, {36 + random.nextInt(48), i * 0.0625, 0.0625 + random.nextInt(8) * 0.0625,
                                               1 + random.nextInt(127), false});
                        w.shell->selection().set({SessionSelection::Kind::slot, 0, 0});
                        w.shell->showClipView();
                        w.settle();
                        auto& roll = w.shell->clipView().pianoRoll();
                        roll.zoomToClip();
                        roll.setZoomX(30.0, 0.0); // about 2000 notes across
                        auto target = std::make_shared<juce::Image>(juce::Image::ARGB, roll.getWidth(), roll.getHeight(), true);
                        return {nullptr, [&roll, target](int frame) {
                            if (frame % 2 == 0)
                                roll.setZoomX(roll.layout().pixelsPerQuarter() * (frame % 40 < 20 ? 1.03 : 1.0 / 1.03), 400.0);
                            else
                                roll.setScroll(roll.layout().scrollQuarters() + 0.5, roll.layout().scrollY() + (frame % 40 < 20 ? 3.0 : -3.0));
                            paint(roll, *target);
                        }};
                    }});

    // The Device view on a rack with 64 macros (a Synth in its chain), four macros turning a frame.
    list.push_back({"Device view, rack with 64 macros moving",
                    [](Window& w) -> Frame {
                        auto& set = w.set();
                        auto chain = LiveSet::chainOf(set.track(0));
                        const auto rack = racks::insertRack(set, chain, -1, RackDevice::instrumentRackId);
                        racks::setMacroCount(set, rack, 64);
                        set.insertDevice(racks::devicesOf(racks::addChain(set, rack)), -1, "lattice.synth");
                        w.shell->selection().set({SessionSelection::Kind::track, 0, 0});
                        w.shell->showDeviceView();
                        w.settle();
                        auto& view = w.shell->deviceView();
                        auto target = std::make_shared<juce::Image>(juce::Image::ARGB, view.getWidth(), view.getHeight(), true);
                        return {nullptr, [&w, &view, rack, target](int frame) {
                            for (int m = 0; m < 4; ++m)
                                w.set().setDeviceParameter(rack, "macro" + juce::String((frame * 4 + m) % 64 + 1), static_cast<double>(frame * 2 % 128));
                            paint(view, *target);
                        }};
                    }});

    // The browser filling with 100,000 search results, scrolled each time.
    list.push_back({"Browser, 100000 results",
                    [](Window& w) -> Frame {
                        w.shell->setBrowserVisible(true);
                        auto& browser = w.shell->browserView();
                        auto rows = std::make_shared<std::vector<BrowserModel::Row>>(100000);
                        for (std::size_t i = 0; i < rows->size(); ++i)
                        {
                            auto& item = (*rows)[i].item;
                            item.kind = library::Kind::sample;
                            item.key = "C:/Samples/Kit " + juce::String(static_cast<int>(i / 100)) + "/Hit " + juce::String(static_cast<int>(i)) + ".wav";
                            item.name = "Hit " + juce::String(static_cast<int>(i));
                            item.detail = library::sampleDetail(0.25 + static_cast<double>(i % 7), 48000.0, 1 + static_cast<int>(i % 2));
                        }
                        auto target = std::make_shared<juce::Image>(juce::Image::ARGB, browser.getWidth(), browser.getHeight(), true);
                        auto copy = std::make_shared<std::vector<BrowserModel::Row>>();
                        return {[rows, copy](int) { *copy = *rows; },
                                [&browser, copy, target](int frame) {
                                    browser.model().setRowsForTests(std::move(*copy));
                                    browser.scrollToRow((frame * 3000) % 100000);
                                    paint(browser, *target);
                                }};
                    }});
    return list;
}

Result measure(const Case& c, int frames)
{
    Window window(c.tracks, c.scenes);
    const auto frame = c.make(window);
    for (int i = 0; i < 5; ++i) // caches filled first, as after the window's first frames
    {
        if (frame.before)
            frame.before(-1 - i);
        frame.draw(-1 - i);
    }
    auto ms = runFrames(frame, frames);
    std::sort(ms.begin(), ms.end());
    Result r;
    r.median = ms[ms.size() / 2];
    r.p95 = ms[std::min(ms.size() - 1, ms.size() * 95 / 100)];
    r.worst = ms.back();
    return r;
}

// --- Idle CPU --------------------------------------------------------------------------------------

/** CPU time used so far, in seconds: the whole process's, and the calling thread's. */
struct CpuTimes
{
    double process = 0.0, thread = 0.0;
};

#if JUCE_WINDOWS
/** The cycle counter's rate. Windows' CPU times (GetProcessTimes) are sampled at the clock tick
    (about 16 ms), too coarse for a 1% share; its cycle times are exact, and count the invariant
    time-stamp counter, so a busy spin calibrates them. */
double cyclesPerSecond()
{
    static const double rate = [] {
        ULONG64 fromCycles = 0, toCycles = 0;
        QueryThreadCycleTime(GetCurrentThread(), &fromCycles);
        const auto from = juce::Time::getHighResolutionTicks();
        while (juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - from) < 0.2)
        {
        }
        QueryThreadCycleTime(GetCurrentThread(), &toCycles);
        return static_cast<double>(toCycles - fromCycles) /
               juce::Time::highResolutionTicksToSeconds(juce::Time::getHighResolutionTicks() - from);
    }();
    return rate;
}
#endif

#if JUCE_WINDOWS
/** The module a thread started in (unnamed threads: a driver's, the system's), or "". */
std::string startModule(HANDLE thread)
{
    using Query = LONG(WINAPI*)(HANDLE, int, PVOID, ULONG, PULONG);
    static const auto query = reinterpret_cast<Query>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    void* start = nullptr;
    constexpr int threadQuerySetWin32StartAddress = 9;
    if (query == nullptr || query(thread, threadQuerySetWin32StartAddress, &start, sizeof(start), nullptr) != 0)
        return {};
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(start), &module))
        return {};
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(module, path, MAX_PATH);
    return juce::File(juce::String(path)).getFileName().toStdString();
}

/** Every thread of this process: its name and its CPU time so far, in seconds. */
std::map<DWORD, std::pair<std::string, double>> threadTimes()
{
    std::map<DWORD, std::pair<std::string, double>> out;
    const auto snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return out;
    THREADENTRY32 entry{};
    entry.dwSize = sizeof(entry);
    for (auto more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
    {
        if (entry.th32OwnerProcessID != GetCurrentProcessId())
            continue;
        const auto thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, entry.th32ThreadID);
        if (thread == nullptr)
            continue;
        ULONG64 cycles = 0;
        QueryThreadCycleTime(thread, &cycles);
        std::string name = "thread " + std::to_string(entry.th32ThreadID);
        if (const auto module = startModule(thread); !module.empty())
            name += " (" + module + ")";
        PWSTR description = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &description)) && description != nullptr)
        {
            if (description[0] != 0)
                name = juce::String(description).toStdString();
            LocalFree(description);
        }
        CloseHandle(thread);
        out[entry.th32ThreadID] = {name, static_cast<double>(cycles) / cyclesPerSecond()};
    }
    CloseHandle(snapshot);
    return out;
}
#endif

CpuTimes cpuTimes()
{
    CpuTimes t;
#if JUCE_WINDOWS
    ULONG64 cycles = 0;
    if (QueryProcessCycleTime(GetCurrentProcess(), &cycles))
        t.process = static_cast<double>(cycles) / cyclesPerSecond();
    if (QueryThreadCycleTime(GetCurrentThread(), &cycles))
        t.thread = static_cast<double>(cycles) / cyclesPerSecond();
#else
    timespec ts{};
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) == 0)
        t.process = static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1.0e-9;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0)
        t.thread = static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) * 1.0e-9;
#endif
    return t;
}

/** Calls each step after its delay, one after another, on the message thread; then stops the loop. */
class Steps final : private juce::Timer
{
public:
    void add(int milliseconds, std::function<void()> step) { steps.push_back({milliseconds, std::move(step)}); }
    void run()
    {
        next();
        juce::MessageManager::getInstance()->runDispatchLoop();
    }

private:
    void next()
    {
        if (at < steps.size())
            startTimer(steps[at].first);
        else
            juce::MessageManager::getInstance()->stopDispatchLoop();
    }
    void timerCallback() override
    {
        stopTimer();
        steps[at++].second();
        next();
    }
    std::vector<std::pair<int, std::function<void()>>> steps;
    std::size_t at = 0;
};

constexpr double idleBudget = 0.003; // of one core, above a bare JUCE window

#if JUCE_WINDOWS
/** The busiest threads between two readings (the message thread is the one this runs on). */
void printBusiestThreads(const std::map<DWORD, std::pair<std::string, double>>& from,
                         const std::map<DWORD, std::pair<std::string, double>>& to, double wall)
{
    std::vector<std::pair<double, std::string>> busiest;
    for (const auto& [id, t] : to)
        if (const auto f = from.find(id); f != from.end())
            busiest.push_back({(t.second - f->second.second) / wall, id == GetCurrentThreadId() ? "(message thread)" : t.first});
    std::sort(busiest.begin(), busiest.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (std::size_t i = 0; i < std::min<std::size_t>(busiest.size(), 8); ++i)
        if (busiest[i].first >= 0.0001)
            std::printf("    %-44s %6.2f%%\n", busiest[i].second.c_str(), busiest[i].first * 100.0);
}
#endif

struct Idle
{
    double process = -1.0; // share of one core; < 0: the device didn't open
    double bareWindow = 0.0; // the same, with only a bare JUCE window of the same size
    double bareMessageThread = 0.0;
};

/** Lattice's window idle, then a bare JUCE window's with nothing of Lattice's left running (its
    engine and session gone); prints what it measured. */
Idle measureIdle(double seconds, bool audio, const juce::String& deviceType, bool virtualPush)
{
    juce::PropertiesFile::Options options;
    options.applicationName = "LatticeBench";
    const auto settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lattice-ui-idle-" + juce::Uuid().toString() + ".settings");
    auto settings = std::make_unique<juce::PropertiesFile>(settingsFile, options);
    TuningRegistry tunings;
    auto enginePtr = std::make_unique<AudioEngine>();
    auto sessionPtr = std::make_unique<SessionController>(*enginePtr, tunings); // the app's devices
    auto& engine = *enginePtr;
    auto& session = *sessionPtr;
    // A set as you'd leave it: clips in the grid, a Colundi track, one clip open in the Clip view.
    session.newSet(8, 8);
    session.set().setTrackTuning(1, TuningRegistry::colundiId);
    for (int t = 0; t < 6; ++t)
        for (int c = 0; c < 4; ++c)
            if ((t + c) % 3 != 0)
            {
                auto clip = session.set().createClip(t, c, 4.0);
                for (int n = 0; n < 8; ++n)
                    session.set().addNote(clip, {48 + (n * 5) % 24, n * 0.5, 0.5, 100, false});
            }
    session.flush();
    juce::String deviceInfo = "no audio device";
    if (audio)
    {
        // The app's own setup, read only.
        juce::PropertiesFile::Options appOptions;
        appOptions.applicationName = "Lattice";
        appOptions.storageFormat = juce::PropertiesFile::storeAsXML;
        appOptions.millisecondsBeforeSaving = -1;
        juce::PropertiesFile app(platform::defaultPaths().settingsFile(), appOptions);
        engine.setWorkerThreads(WorkerPool::defaultWorkers());
        auto saved = app.getXmlValue("audioDeviceState");
        if (saved != nullptr && deviceType.isNotEmpty())
            saved->setAttribute("deviceType", deviceType);
        const auto error = engine.openDevice(saved.get());
        if (error.isNotEmpty())
        {
            std::printf("The audio device didn't open: %s\n", error.toRawUTF8());
            return {};
        }
        auto* device = engine.getDeviceManager().getCurrentAudioDevice();
        deviceInfo = device == nullptr ? juce::String("no audio device")
                                       : device->getTypeName() + " " + device->getName() + ", " + juce::String(device->getCurrentBufferSizeSamples()) +
                                             " samples at " + juce::String(device->getCurrentSampleRate(), 0) + " Hz, " +
                                             juce::String(engine.workerThreads()) + " worker threads";
    }

    auto shell = std::make_unique<MainShell>(session, engine, *settings, tunings, ShellActions{});
    shell->setDetailVisible(true);
    shell->setBrowserVisible(true);
    const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay();
    shell->setBounds(display != nullptr ? display->userBounds.reduced(display->userBounds.getWidth() / 10.0f).toNearestInt() : juce::Rectangle<int>(1280, 800));
    shell->addToDesktop(juce::ComponentPeer::windowAppearsOnTaskbar | juce::ComponentPeer::windowHasTitleBar);
    shell->setVisible(true);
    shell->selection().set({SessionSelection::Kind::slot, 0, 1});
    shell->showClipView();
    if (virtualPush)
        shell->setVirtualPushVisible(true);

#if JUCE_WINDOWS
    cyclesPerSecond(); // calibrated before, not during
#endif
    CpuTimes from, to;
    double tickWork = 0.0;
#if JUCE_WINDOWS
    std::map<DWORD, std::pair<std::string, double>> threadsFrom, threadsTo;
#endif
    double fromWall = 0.0, toWall = 0.0;
    std::uint64_t fromTicks = 0, ticks = 0;
    AudioCallbackStats::Summary paints, callbacks;
    Steps steps;
    steps.add(3000, [&] { // settled: the window drawn, the library indexed, the device started
#if JUCE_WINDOWS
        threadsFrom = threadTimes();
#endif
        from = cpuTimes();
        fromWall = juce::Time::getMillisecondCounterHiRes();
        fromTicks = shell->feed().ticks();
        shell->feed().takeTickWorkSeconds();
        shell->resetLoadStatistics();
    });
    steps.add(juce::roundToInt(seconds * 1000.0), [&] {
        to = cpuTimes();
        toWall = juce::Time::getMillisecondCounterHiRes();
#if JUCE_WINDOWS
        threadsTo = threadTimes();
#endif
        ticks = shell->feed().ticks() - fromTicks;
        tickWork = shell->feed().takeTickWorkSeconds(); // all of it: nothing painted to take it
        paints = shell->frameStats().summary();
        callbacks = engine.callbackStats().summary();
    });
    std::unique_ptr<juce::Component> bare;
    CpuTimes bareFrom, bareTo;
#if JUCE_WINDOWS
    std::map<DWORD, std::pair<std::string, double>> bareThreadsFrom, bareThreadsTo;
#endif
    double bareFromWall = 0.0, bareToWall = 0.0;
    steps.add(1, [&] { // straight after
        const auto bounds = shell->getBounds();
        shell.reset();
        engine.getDeviceManager().closeAudioDevice();
        sessionPtr.reset();
        enginePtr.reset();
        bare = std::make_unique<juce::Component>();
        bare->setBounds(bounds);
        bare->addToDesktop(juce::ComponentPeer::windowAppearsOnTaskbar | juce::ComponentPeer::windowHasTitleBar);
        bare->setVisible(true);
    });
    steps.add(3000, [&] {
#if JUCE_WINDOWS
        bareThreadsFrom = threadTimes();
#endif
        bareFrom = cpuTimes();
        bareFromWall = juce::Time::getMillisecondCounterHiRes();
    });
    steps.add(juce::roundToInt(seconds * 1000.0), [&] {
        bareTo = cpuTimes();
#if JUCE_WINDOWS
        bareThreadsTo = threadTimes();
#endif
        bareToWall = juce::Time::getMillisecondCounterHiRes();
    });
    steps.run();
    bare.reset();
    settings.reset();
    settingsFile.deleteFile();

    const double wall = (toWall - fromWall) / 1000.0;
    const double process = (to.process - from.process) / wall;
    const double thread = (to.thread - from.thread) / wall;
    std::printf("Idle CPU over %.1f s (%s), share of one core:\n", wall, deviceInfo.toRawUTF8());
    std::printf("  %-28s %6.2f%%\n", "process", process * 100.0);
    std::printf("  %-28s %6.2f%%\n", "message thread", thread * 100.0);
    std::printf("  %-28s %6.2f%%  (%.1f us a tick)\n", "the feed's ticks", tickWork / wall * 100.0,
                ticks > 0 ? tickWork / static_cast<double>(ticks) * 1.0e6 : 0.0);
    if (callbacks.callbacks > 0)
        std::printf("  %-28s %6.2f%%  (the median callback's share of its period)\n", "audio callbacks", callbacks.p50 * 100.0);
#if JUCE_WINDOWS
    std::printf("  by thread:\n");
    printBusiestThreads(threadsFrom, threadsTo, wall);
#endif
    std::printf("  %.0f refresh ticks a second; the window painted %llu times in all, %llu of them over the frame budget\n",
                static_cast<double>(ticks) / wall, static_cast<unsigned long long>(paints.callbacks),
                static_cast<unsigned long long>(paints.overruns));

    const double bareWall = (bareToWall - bareFromWall) / 1000.0;
#if JUCE_WINDOWS
    std::printf("  a bare JUCE window, by thread (its message thread %.2f%%):\n", (bareTo.thread - bareFrom.thread) / bareWall * 100.0);
    printBusiestThreads(bareThreadsFrom, bareThreadsTo, bareWall);
#endif
    return {process, (bareTo.process - bareFrom.process) / bareWall, (bareTo.thread - bareFrom.thread) / bareWall};
}
} // namespace

int main(int argc, char* argv[])
{
    int frames = 120;
    std::string filter;
    double idleSeconds = 0.0;
    bool audio = false;
    juce::String deviceType;
    bool virtualPush = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            frames = std::max(10, std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc)
            filter = argv[++i];
        else if (std::strcmp(argv[i], "--idle") == 0 && i + 1 < argc)
            idleSeconds = std::max(1.0, std::atof(argv[++i]));
        else if (std::strcmp(argv[i], "--audio") == 0)
            audio = true;
        else if (std::strcmp(argv[i], "--virtual-push") == 0)
            virtualPush = true;
        else if (std::strcmp(argv[i], "--device-type") == 0 && i + 1 < argc)
            deviceType = juce::String::fromUTF8(argv[++i]);
    }
    const juce::ScopedJuceInitialiser_GUI juceInit;
    if (idleSeconds > 0.0)
    {
        const auto idle = measureIdle(idleSeconds, audio, deviceType, virtualPush);
        if (idle.process < 0.0)
            return 1;
        const double above = idle.process - idle.bareWindow;
        std::printf("  %-28s %6.2f%%  (JUCE's floor: measured next, nothing of Lattice's running)\n", "a bare JUCE window",
                    idle.bareWindow * 100.0);
        std::printf("  %-28s %6.2f%%\n", "  its message thread", idle.bareMessageThread * 100.0);
        if (audio)
        {
            std::printf("  %-28s %6.2f%%  (not judged with the device open)\n", "above it", above * 100.0);
            return 0;
        }
        const bool within = above <= idleBudget;
        std::printf("  %-28s %6.2f%%  (budget %.2f%%)%s\n", "above it", above * 100.0, idleBudget * 100.0, within ? "" : "  OVER");
        return within ? 0 : 1;
    }
    std::printf("%-42s %9s %9s %9s %8s\n", "UI frame (ms)", "median", "p95", "worst", "budget");
    bool allWithin = true;
    for (const auto& c : cases())
    {
        if (!filter.empty() && c.name.find(filter) == std::string::npos)
            continue;
        const auto r = measure(c, frames);
        const bool within = r.p95 <= budgetMs;
        allWithin = allWithin && within;
        std::printf("%-42s %9.3f %9.3f %9.3f %8.1f%s\n", c.name.c_str(), r.median, r.p95, r.worst, budgetMs, within ? "" : "  OVER");
    }
    return allWithin ? 0 : 1;
}
