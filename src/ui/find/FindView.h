#pragma once

#include "ui/find/FindIndex.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lattice::ui
{

/** The Find box (docs/05-m4-ui.md, 4g): a search field at the top of the window with the results
    under it, as in a web browser. Results come as you type; Up and Down move, Enter (or a click)
    takes you to the one chosen (onChoose), Esc closes. Opened for commands only it's a command
    palette (Ctrl+Shift+P). */
class FindView final : public juce::Component, private juce::TextEditor::Listener, private juce::KeyListener
{
public:
    explicit FindView(FindIndex& index);
    ~FindView() override;

    /** Shows it with an empty field and the keys in it. */
    void open(bool commandsOnly);
    void close();
    bool isCommandPalette() const noexcept { return commandsOnly; }

    void setText(const juce::String& text);
    const std::vector<FindIndex::Result>& results() const noexcept { return shown; }
    int selectedRow() const noexcept { return selected; }
    void moveSelection(int delta);
    /** Takes you to the selected result (and closes). */
    void chooseSelected();

    /** Whose results come first (the selected track). */
    std::function<int()> selectedTrack;
    std::function<void(const FindIndex::Result&)> onChoose;
    std::function<void()> onClose;

    static constexpr int maxResults = 60;
    /** The height it wants for its results (the theme's sizes). */
    int preferredHeight() const;

    /** The chosen result, for screen readers (said as Up and Down move): "Release, Synth › Amp
        Env · 2 MIDI, 2 of 14". */
    juce::String describeSelected() const;
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override;

    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails&) override;

private:
    void textEditorTextChanged(juce::TextEditor&) override { refresh(); }
    void textEditorReturnKeyPressed(juce::TextEditor&) override { chooseSelected(); }
    void textEditorEscapeKeyPressed(juce::TextEditor&) override { close(); }
    bool keyPressed(const juce::KeyPress& key, juce::Component*) override;
    void refresh();
    int rowHeight() const;
    int visibleRows() const;
    juce::Rectangle<int> rowArea(int visibleIndex) const;

    FindIndex& index;
    juce::TextEditor field;
    std::vector<FindIndex::Result> shown;
    int selected = 0;
    int firstShown = 0;
    bool commandsOnly = false;
};

} // namespace lattice::ui
