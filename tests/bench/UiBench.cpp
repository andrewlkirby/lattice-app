// UI benchmarks (docs/05-m4-ui.md, Tests 10; docs/09-performance-charter.md "UI responsiveness"):
// the views that take the most drawing, frame by frame as the window would draw them, against the
// charter's 8 ms frame time while busy.
//
//   lattice_ui_bench [--frames N] [--filter name]
//
// Each case paints its view headless (JUCE's software renderer into an image: the app's own
// renderer on Windows is Direct2D, usually faster) once per frame while the view changes, and
// reports the median, the 95th percentile and the worst frame. What isn't the UI's work (the
// engine rendering the audio the meters show, a test's copy of its rows) happens between frames,
// untimed. A case is over budget when its 95th
// percentile is above 8 ms. The run fails (exit code 1) if any case is over. scripts/linux-test.sh
// --callgrind counts the instructions in runFrames() against docs/perf/baselines/ui-callgrind.txt.

#include "devices/racks/Rack.h"
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
        settingsFile = juce::File::getSpecialLocation(juce::File::tempDirectory).getNonexistentChildFile("lattice-ui-bench", ".settings");
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
} // namespace

int main(int argc, char* argv[])
{
    int frames = 120;
    std::string filter;
    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            frames = std::max(10, std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--filter") == 0 && i + 1 < argc)
            filter = argv[++i];
    }
    const juce::ScopedJuceInitialiser_GUI juceInit;
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
