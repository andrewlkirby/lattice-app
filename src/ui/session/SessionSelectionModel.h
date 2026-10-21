#pragma once

#include "ui/session/SessionSelection.h"

#include <juce_events/juce_events.h>

namespace lattice::ui
{

/** The Session view's selection, shared by everything that acts on it: the views that show it, the
    commands (Duplicate, Delete, Launch...), and, until 4d, the debug UI's device editor, which
    follows the selected track. Listeners hear about every change, synchronously. Message thread. */
class SessionSelectionModel
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void selectionChanged(const SessionSelection& now) = 0;
    };

    const SessionSelection& get() const noexcept { return current; }
    /** Changes the selection (no-op if it's the same) and tells the listeners. */
    void set(const SessionSelection& s);

    void addListener(Listener* l) { listeners.add(l); }
    void removeListener(Listener* l) { listeners.remove(l); }

private:
    SessionSelection current;
    juce::ListenerList<Listener> listeners;
};

inline void SessionSelectionModel::set(const SessionSelection& s)
{
    if (s == current)
        return;
    current = s;
    listeners.call([this](Listener& l) { l.selectionChanged(current); });
}

} // namespace lattice::ui
