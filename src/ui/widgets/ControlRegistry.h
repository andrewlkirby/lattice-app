#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <vector>

namespace lattice::ui
{

class ValueBinding;

/** Every control the user can reach (docs/05-m4-ui.md, "Widget library"): what MIDI Map, Key Map
    and Find (4g) search. A bound widget registers itself while it's in a window under a component
    that provides a registry (the shell); views add how to reveal a control that isn't showing
    (select the track, open the device) as they're built. Message thread only. */
class ControlRegistry
{
public:
    struct Entry
    {
        juce::Component::SafePointer<juce::Component> component = {};
        const ValueBinding* binding = nullptr; // the value it controls (null for a command button)
        juce::String name = {};                // "Release"
        juce::String group = {};               // "Amp Env"
        juce::String owner = {};               // "Synth · 2 MIDI" (the view fills it in)
        /** Brings the control on screen (empty: it's on screen whenever it exists). */
        std::function<void()> reveal = {};
    };

    /** A component that hands a registry to the widgets inside it. */
    struct Provider
    {
        virtual ~Provider() = default;
        virtual ControlRegistry& controlRegistry() = 0;
    };

    /** The registry of the nearest provider above `component`, or nullptr. */
    static ControlRegistry* findFor(const juce::Component& component);

    void add(Entry entry);
    void remove(const juce::Component* component);
    /** The entries whose component still exists. */
    std::vector<const Entry*> entries() const;
    const Entry* find(const juce::Component* component) const;

    /** Entries whose name or group contains every word of `text` (as Find matches; 4g ranks them). */
    std::vector<const Entry*> search(const juce::String& text) const;

private:
    std::vector<Entry> list;
};

} // namespace lattice::ui
