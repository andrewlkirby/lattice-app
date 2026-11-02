#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <unordered_map>
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

    /** A component that says where the controls inside it live (a device's frame: "Synth ·
        2 MIDI", and how to bring it into view). The nearest one above a control describes it. */
    struct Locator
    {
        virtual ~Locator() = default;
        /** Fills in the entry's owner and reveal (and may refine its name or group). */
        virtual void describe(Entry& entry) const = 0;
    };

    /** The registry of the nearest provider above `component`, or nullptr. */
    static ControlRegistry* findFor(const juce::Component& component);
    /** What a widget calls when its place in the window changes: (re)registers it with the
        registry it's in now, described by the nearest Locator, and leaves the one it was in.
        `current` is the widget's own record of where it's registered. */
    static void place(juce::Component& control, const ValueBinding* binding, ControlRegistry*& current);

    void add(Entry entry);
    void remove(const juce::Component* component);
    /** The entries whose component still exists. */
    std::vector<const Entry*> entries() const;
    const Entry* find(const juce::Component* component) const;

    /** Entries whose name or group contains every word of `text` (as Find matches; 4g ranks them). */
    std::vector<const Entry*> search(const juce::String& text) const;

private:
    std::unordered_map<const juce::Component*, Entry> list;
};

} // namespace lattice::ui
