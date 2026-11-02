#pragma once

#include "engine/SessionController.h"
#include "ui/clip/PianoRoll.h"
#include "ui/widgets/NumberBox.h"
#include "ui/widgets/Toggle.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace lattice::ui
{

/** The Clip view's left side (docs/05-m4-ui.md, 4c), in sections as in Live:
    - Clip: name, colour, time signature (the piano roll's bars; launching follows the set's).
    - Launch: mode, quantization, legato, and follow actions (A and B, A's chance, the time).
    - Loop: on or off, the start marker, the end, and the loop's position and length, in
      bars.beats.sixteenths.
    - Notes: the note tools (quantize with its amount, legato, reverse, invert, stretch, transpose
      by table steps or semitones), the loop tools (double, halve, duplicate), and the piano roll's
      modes (draw, fold, log-spaced rows).
    Every value is a bound widget, so each change is one undo step and undo shows in it. The
    buttons don't take the keyboard focus, so the piano roll keeps the keys. */
class ClipPropertiesPanel final : public juce::Component, private juce::ValueTree::Listener
{
public:
    ClipPropertiesPanel(SessionController& session, PianoRoll& roll);
    ~ClipPropertiesPanel() override;

    /** Rebuilds for a clip (an invalid tree: nothing). */
    void setClip(const juce::ValueTree& clip);
    /** The height the controls need at a width. */
    int preferredHeight() const;

    /** A control by its title ("Loop Length"), for tests (nullptr if none). */
    juce::Component* control(const juce::String& title) const;
    /** Buttons by their caption ("Legato"), for tests. */
    juce::Button* button(const juce::String& caption) const;

    void paint(juce::Graphics&) override;
    void resized() override;

private:
    struct Row
    {
        juce::String label;
        std::vector<juce::Component*> controls; // side by side
        bool heading = false;
    };

    void build();
    void heading(const juce::String& text);
    void row(const juce::String& label, std::vector<juce::Component*> controls);
    NumberBox* box(std::unique_ptr<ValueBinding> binding);
    juce::Button* action(const juce::String& caption, const juce::String& help, std::function<void()> run);
    Toggle* mode(const juce::String& caption, const juce::String& help, bool on, std::function<void(bool)> set);
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    int rowHeight() const;

    SessionController& session;
    PianoRoll& roll;
    juce::ValueTree clip;
    std::vector<std::unique_ptr<juce::Component>> owned;
    std::vector<Row> rows;
    class NameButton;
    class ColourSwatch;
};

} // namespace lattice::ui
