#pragma once

#include "ui/clip/ClipPropertiesPanel.h"
#include "ui/clip/PianoRoll.h"
#include "ui/session/SessionSelectionModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lattice::ui
{

/** The Clip view (docs/05-m4-ui.md, 4c): the selected clip's properties on the left and its piano
    roll on the right. It shows the clip in the Session view's selected slot (the first one of a
    range), and follows it when it's created, deleted, moved by undo, or replaced. */
class ClipView final : public juce::Component,
                       private SessionSelectionModel::Listener,
                       private juce::ValueTree::Listener,
                       private juce::AsyncUpdater
{
public:
    ClipView(SessionController& session, EngineStateFeed& feed, TuningRegistry& tunings, SessionSelectionModel& selection);
    ~ClipView() override;

    PianoRoll& pianoRoll() noexcept { return roll; }
    ClipPropertiesPanel& properties() noexcept { return panel; }
    /** The clip shown (an invalid tree if none). */
    const juce::ValueTree& clip() const noexcept { return roll.clip(); }

    /** The piano roll, when it has the keys: it takes the Edit commands then. */
    EditTarget* editTargetIfFocused();

    /** Applies pending changes now (tests). */
    void flushUpdates();

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    void selectionChanged(const SessionSelection&) override { triggerAsyncUpdate(); }
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { triggerAsyncUpdate(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { triggerAsyncUpdate(); }
    void valueTreeParentChanged(juce::ValueTree&) override { triggerAsyncUpdate(); }
    void valueTreeRedirected(juce::ValueTree&) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override;
    void follow();

    SessionController& session;
    SessionSelectionModel& selection;
    PianoRoll roll;
    ClipPropertiesPanel panel;
    juce::Viewport panelView;
    juce::ValueTree watchedSlot;
};

} // namespace lattice::ui
