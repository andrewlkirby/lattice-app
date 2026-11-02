#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace lattice::ui
{

/** The undo history (docs/05-m4-ui.md, 4g), in the side column under the browser: every step, the
    oldest first, from "Start" (the set as it was opened); the steps that can be redone follow,
    dimmed. Each step reads what was done, then where, dimmed (undoNames). Clicking a step undoes
    or redoes to just after it; with the keys, Up and Down step back and forward, Home and End go to
    the start and the latest. Follows the undo manager as it changes. Message thread only. */
class UndoHistoryView final : public juce::Component, private juce::ChangeListener
{
public:
    explicit UndoHistoryView(juce::UndoManager& undo);
    ~UndoHistoryView() override;

    /** The rows: "Start", the steps done (oldest first), then those that can be redone. */
    const juce::StringArray& rows() const noexcept { return names; }
    /** The row the set is at now (0: Start). */
    int current() const noexcept { return at; }
    /** Undoes or redoes until the set is just after row `row`. */
    void goTo(int row);
    /** Reads the history again (the undo manager announces changes asynchronously). */
    void refresh();

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    bool keyPressed(const juce::KeyPress&) override;
    void focusGained(FocusChangeType) override { repaint(); }
    void focusLost(FocusChangeType) override { repaint(); }
    /** Screen readers: a list whose value is the step the set is at ("Cutoff → 1.20 kHz · Synth
        · 1 MIDI, step 4 of 6"). */
    juce::String describeCurrent() const;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }
    int rowHeight() const;

    juce::UndoManager& undo;
    juce::StringArray names;
    int at = 0;
    int scrollRows = 0; // the first row shown
};

} // namespace lattice::ui
