#pragma once

#include "ui/shell/MainShell.h"

#include <juce_events/juce_events.h>

namespace lattice
{

/** The UI stress script (docs/05-m4-ui.md, Tests 11): works the window as fast as it draws while
    the engine plays, so the dropout soak (SoakTest, `--ui-stress`) can show that the UI never
    takes the audio's time. Each frame it does one thing of the current phase, and the phases
    take turns every few seconds:
    - scrolling and zooming the Session grid
    - zooming and scrolling the piano roll, on a clip of each track in turn
    - turning a device's knob (the Synth's cutoff) in the Device view
    - moving mixer faders
    - switching views: Clip, Device and Tuning pages, the browser, the undo history, Find
    - the selection moving from track to track (the Device view rebuilds each time)
    A theme change starts each round. A new scene launches every 30 s (as in the engine's stress
    test), and the undo history is cleared between phases (it would otherwise grow by 60 steps a
    second). Message thread only. */
class UiStress final : private juce::Timer
{
public:
    explicit UiStress(ui::MainShell& shell);
    ~UiStress() override;

    void start();
    /** The phases completed, for the log. */
    int rounds() const noexcept { return round; }
    /** The current step, for the soak's dropout lines: e.g. "round 3, piano roll, frame 120, scene 2". */
    juce::String describe() const;

private:
    void timerCallback() override;
    void step(int phase, int frame);

    ui::MainShell& shell;
    int frame = 0;
    int round = 0;
    int scene = 0;
    double startedSeconds = 0.0;
};

} // namespace lattice
