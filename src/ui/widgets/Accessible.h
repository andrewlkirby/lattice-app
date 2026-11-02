#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>

namespace lattice::ui
{

/** Screen-reader support for the views that draw their own items (docs/05-m4-ui.md, 4h): the
    Session grid, the piano roll, the browser's list, the undo history, Find's results, the tuning
    table, the Device view. Such a view reads as one element of `role`, named by its title; its
    value says what's selected ("2 MIDI · Scene 3: Bass, playing"), and when that changes while it
    has the keyboard focus, the change is announced. Message thread only. */
namespace accessible
{
/** A handler for a view whose value is `describe()` (read-only text). */
std::unique_ptr<juce::AccessibilityHandler> viewHandler(juce::Component& view, juce::AccessibilityRole role,
                                                        std::function<juce::String()> describe);

/** What a view calls when its selection (and so its value) changed. */
void selectionChanged(juce::Component& view);

/** The value a view's handler reports now (empty if it has none). */
juce::String valueOf(juce::Component& view);

/** Marks a view that takes the arrows itself (the Session grid, the piano roll, a list): in it,
    Shift+Tab is the shell's Clip/Device switch, as in Live. Anywhere else (a knob, a button, a
    text field reached with Tab) Shift+Tab goes back to the control before. */
void markAsView(juce::Component& view);
bool isView(const juce::Component* component);

/** Gives the keys to the first control inside `area` that takes them (in Tab's order). False if
    there's none. */
bool focusFirstIn(juce::Component& area);
} // namespace accessible

} // namespace lattice::ui
