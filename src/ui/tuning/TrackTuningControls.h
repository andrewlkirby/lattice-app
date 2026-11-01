#pragma once

#include "engine/SessionController.h"
#include "ui/widgets/NumberBox.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>

namespace lattice::ui
{

/** A MIDI track's tuning settings as number boxes (docs/05-m4-ui.md, 4f), for the mixer's I/O
    section and the Tuning view:

    - Tuning: every tuning the registry lists (a missing one the track names is offered as such);
    - High Notes: Mute or Fold Down, for notes too high to play at the sample rate;
    - Bend: in Cents or table Steps, and its range (each mode keeps its own; the box shown is the
      mode's).

    Each is a bound widget: one undo step per gesture, following the document. The tuning list
    follows the registry (a file loaded or saved shows up). */
class TrackTuningControls final : public juce::Component, private TuningRegistry::Listener, private juce::ValueTree::Listener
{
public:
    enum class Layout
    {
        stacked, // three rows: tuning; high notes; bend mode and range (a mixer strip)
        row      // one row, left to right (the Tuning view's bar)
    };

    TrackTuningControls(SessionController& session, TuningRegistry& tunings, int track, Layout layout);
    ~TrackTuningControls() override;

    int track() const noexcept { return trackIndex; }
    static constexpr int stackedRows = 3;
    /** The width the row layout wants (the theme's sizes). */
    int rowWidth() const;

    NumberBox* tuningBox() noexcept { return tuning.get(); }
    NumberBox* highNotesBox() noexcept { return highNotes.get(); }
    NumberBox* bendModeBox() noexcept { return bendMode.get(); }
    /** The range box of the track's bend mode (the one shown). */
    NumberBox* bendRangeBox() noexcept { return stepsMode() ? rangeSteps.get() : rangeCents.get(); }

    void resized() override;

private:
    void tuningsChanged() override;
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void makeTuningBox();
    bool stepsMode() const;

    SessionController& session;
    TuningRegistry& registry;
    int trackIndex;
    Layout layout;
    juce::ValueTree node;
    std::unique_ptr<NumberBox> tuning, highNotes, bendMode, rangeCents, rangeSteps;
};

} // namespace lattice::ui
