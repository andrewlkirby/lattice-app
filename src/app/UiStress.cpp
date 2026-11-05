#include "app/UiStress.h"

#include "ui/clip/ClipView.h"
#include "ui/clip/PianoRoll.h"
#include "ui/find/FindView.h"
#include "ui/session/SessionView.h"

#include <cmath>

namespace lattice
{
namespace
{
constexpr int framesPerPhase = 240; // 4 s at 60 frames a second
constexpr int numPhases = 7;
constexpr double secondsPerScene = 30.0;

/** 0 to 1 and back over `period` frames. */
double wave(int frame, int period)
{
    return 0.5 - 0.5 * std::cos(2.0 * juce::MathConstants<double>::pi * frame / period);
}
} // namespace

UiStress::UiStress(ui::MainShell& s) : shell(s) {}

UiStress::~UiStress()
{
    stopTimer();
}

void UiStress::start()
{
    startedSeconds = juce::Time::getMillisecondCounterHiRes() * 0.001;
    shell.setVirtualPushVisible(true);
    shell.deviceContext().session.launchScene(0);
    startTimer(16);
}

void UiStress::timerCallback()
{
    auto& session = shell.deviceContext().session;
    const double elapsed = juce::Time::getMillisecondCounterHiRes() * 0.001 - startedSeconds;
    if (const int due = static_cast<int>(elapsed / secondsPerScene); due != scene && session.set().numScenes() > 0)
    {
        scene = due;
        session.launchScene(scene % session.set().numScenes());
    }

    const int phase = (frame / framesPerPhase) % numPhases;
    const int inPhase = frame % framesPerPhase;
    if (inPhase == 0)
    {
        session.undoManager().clearUndoHistory();
        if (phase == 0)
        {
            const ui::Theme* themes[] = {&ui::Theme::dark(), &ui::Theme::light(), &ui::Theme::highContrast()};
            shell.setTheme(*themes[round % 3]);
            ++round;
        }
    }
    step(phase, inPhase);
    ++frame;
}

juce::String UiStress::describe() const
{
    static constexpr const char* names[numPhases] = {"session grid", "piano roll", "device knob", "mixer faders", "views", "selection",
                                                     "virtual push"};
    const int done = juce::jmax(0, frame - 1); // the frame last run
    return "round " + juce::String(round) + ", " + names[(done / framesPerPhase) % numPhases] + ", frame " +
           juce::String(done % framesPerPhase) + ", scene " + juce::String(scene);
}

void UiStress::step(int phase, int f)
{
    auto& session = shell.deviceContext().session;
    auto& set = session.set();
    const int tracks = set.numTracks();
    if (tracks == 0)
        return;
    auto& grid = shell.sessionView();
    switch (phase)
    {
    case 0: // the Session grid: scrolling both ways, and zooming
        grid.setScrollPosition({juce::roundToInt(wave(f, 90) * 2000.0), juce::roundToInt(wave(f, 70) * 400.0)});
        if (f % 30 == 0)
            grid.setZoom(static_cast<float>(0.7 + wave(f, 120)), static_cast<float>(0.7 + wave(f + 60, 120)));
        break;
    case 1: // the piano roll on a clip of each track in turn
    {
        if (f % 60 == 0)
        {
            const int track = (f / 60 + round) % tracks;
            shell.selection().set({ui::SessionSelection::Kind::slot, track, scene % juce::jmax(1, set.numScenes())});
            shell.showClipView();
        }
        auto& roll = shell.clipView().pianoRoll();
        roll.setZoomX(20.0 + wave(f, 80) * 200.0, roll.getWidth() * 0.5);
        roll.setScroll(wave(f, 100) * 8.0, wave(f, 60) * 600.0);
        if (f % 20 == 0)
            roll.setRowZoom(static_cast<float>(0.6 + wave(f, 120) * 2.0));
        break;
    }
    case 2: // a knob turning in the Device view
    {
        const int track = round % tracks;
        if (f == 0)
        {
            shell.selection().set({ui::SessionSelection::Kind::track, track, 0});
            shell.showDeviceView();
        }
        const auto chain = LiveSet::chainOf(set.track(track));
        if (chain.getNumChildren() > 0)
            set.setDeviceParameter(chain.getChild(0), "filter.cutoff", 200.0 + wave(f, 50) * 8000.0);
        break;
    }
    case 3: // mixer faders
        for (int t = 0; t < juce::jmin(tracks, 8); ++t)
            set.setTrackVolumeDb(t, -24.0 + wave(f + t * 10, 60) * 24.0);
        break;
    case 4: // views opening and closing
        switch ((f / 15) % 8)
        {
        case 0: shell.showClipView(); break;
        case 1: shell.showDeviceView(); break;
        case 2: shell.showTuningView(); break;
        case 3: shell.setBrowserVisible(!shell.browserView().isVisible()); break;
        case 4: shell.setUndoHistoryVisible(!shell.isUndoHistoryVisible()); break;
        case 5: shell.openFind(false); break;
        case 6: shell.findView().close(); break;
        default: shell.setDetailVisible(true); break;
        }
        break;
    case 5: // the selection moving from track to track (each Device view rebuilds)
        if (f % 6 == 0)
        {
            shell.selection().set({ui::SessionSelection::Kind::track, (f / 6) % tracks, 0});
            shell.showDeviceView();
        }
        break;
    default: // Virtual Push: a mode a second, pads pressed and let go, encoders turning
    {
        auto* push = shell.virtualPushView();
        if (push == nullptr)
            break;
        using control::SurfaceInput;
        const auto& caps = push->capabilities();
        if (f % 60 == 0)
        {
            static constexpr control::Role modes[] = {control::Role::modeSession, control::Role::modeNote, control::Role::modeDevice,
                                                      control::Role::modeMix};
            const auto role = modes[(f / 60) % 4];
            push->deliver(SurfaceInput::buttonDown({role, 0}));
            push->deliver(SurfaceInput::buttonUp({role, 0}));
        }
        if (caps.numPads() > 0)
        {
            const int pad = (f * 7 + round) % caps.numPads();
            const int was = ((f - 4) * 7 + round) % caps.numPads();
            if (f % 4 == 0)
                push->deliver(SurfaceInput::padDown(pad / caps.columns, pad % caps.columns, 0.8f));
            if (f >= 4 && f % 4 == 0)
                push->deliver(SurfaceInput::padUp(was / caps.columns, was % caps.columns));
        }
        if (caps.encoders > 0)
            push->deliver(SurfaceInput::encoderTurn(f % caps.encoders, (f / 20) % 2 == 0 ? 3 : -3));
        if (f == framesPerPhase - 1)
            for (int p = 0; p < caps.numPads(); ++p)
                push->deliver(SurfaceInput::padUp(p / caps.columns, p % caps.columns)); // none left held
        break;
    }
    }
}

namespace
{
juce::String summaryOf(const LatencyStats& stats)
{
    const auto s = stats.summary();
    return "n=" + juce::String(static_cast<juce::int64>(s.count)) + " p50=" + juce::String(s.p50Ms, 2) + " p99=" + juce::String(s.p99Ms, 2) +
           " max=" + juce::String(s.maxMs, 2);
}
} // namespace

juce::String UiStress::report()
{
    auto* controller = shell.virtualPushController();
    auto* push = shell.virtualPushView();
    if (controller == nullptr || push == nullptr)
        return " | virtual push: not shown";
    return " | virtual push response ms (to its lights) " + summaryOf(controller->responseTimes()) + " | its refresh takes ms " +
           summaryOf(controller->refreshCost()) + " | its paint ms " +
           summaryOf(push->paintDelays()) + " (a frame: 16.67) | its paint takes ms " + summaryOf(push->paintCost());
}

void UiStress::resetMeasures()
{
    if (auto* controller = shell.virtualPushController())
    {
        controller->responseTimes().reset();
        controller->refreshCost().reset();
    }
    if (auto* push = shell.virtualPushView())
    {
        push->paintDelays().reset();
        push->paintCost().reset();
    }
}

} // namespace lattice
