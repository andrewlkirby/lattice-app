#pragma once

#include "ui/mapping/Mappings.h"
#include "ui/session/SessionView.h"
#include "ui/widgets/ControlRegistry.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lattice::ui
{

/** What MIDI Map and Key Map mode show over the window (docs/05-m4-ui.md, 4g): every control that
    can be mapped is tinted, with what it's mapped to written on it; the chosen one is outlined.
    A click chooses a control, or a clip slot or scene in the Session view, as what the next note,
    CC or key maps to (clicks don't reach the controls meanwhile). In Key Map mode it has the keys:
    a key maps; Esc leaves the mode. */
class MapOverlay final : public juce::Component
{
public:
    MapOverlay(Mappings& mappings, ControlRegistry& controls, SessionView& session);

    /** The target and its name under a point (local), if any: a control, a slot or a scene. */
    std::pair<juce::String, juce::String> targetAt(juce::Point<int> local) const;
    /** Chooses what's under a point. */
    void choose(juce::Point<int> local);

    std::function<void()> onExit;
    /** Keys that aren't learnt in Key Map mode (the shortcuts that leave it). */
    std::function<bool(const juce::KeyPress&)> passesThrough;

    void paint(juce::Graphics&) override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseMove(const juce::MouseEvent&) override;
    bool keyPressed(const juce::KeyPress&) override;

private:
    juce::Rectangle<int> boundsOf(const juce::Component& c) const;

    Mappings& mappings;
    ControlRegistry& controls;
    SessionView& sessionView;
    juce::Point<int> hover{-1, -1};
};

/** The mappings, in the side column while a map mode is on: each one's source and target, its
    range and how it takes over (right-click), and Remove; "Map a Command..." chooses a command to
    map; the heading says what to do next. */
class MappingsView final : public juce::Component
{
public:
    MappingsView(Mappings& mappings, CommandRegistry& commands);

    void refresh();
    void paint(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;

    /** The menu a right-click on a row shows (public for tests). */
    juce::PopupMenu menuFor(const juce::ValueTree& mapping);

private:
    int rowHeight() const;

    Mappings& mappings;
    CommandRegistry& commands;
    std::vector<juce::ValueTree> rows;
    juce::TextButton mapCommand{"Map a Command..."};
    juce::Rectangle<int> listArea, headingArea;
};

} // namespace lattice::ui
