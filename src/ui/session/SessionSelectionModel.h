#pragma once

#include "ui/session/SessionSelection.h"

#include <juce_events/juce_events.h>

namespace lattice::ui
{

/** Whose devices the Device view shows: a track's, a return's or the master's. It follows the
    selected track; clicking a return's header or the master shows theirs. */
struct DeviceOwner
{
    enum class Kind
    {
        track,
        returnTrack,
        master
    };
    Kind kind = Kind::track;
    int index = 0; // the track's or return's index (0 for the master)

    bool operator==(const DeviceOwner&) const = default;
};

/** The Session view's selection, shared by everything that acts on it: the views that show it, the
    commands (Duplicate, Delete, Launch...), and the Device view, which shows the selected track's
    devices (or a return's, or the master's). Listeners hear about every change, synchronously.
    Message thread. */
class SessionSelectionModel
{
public:
    struct Listener
    {
        virtual ~Listener() = default;
        virtual void selectionChanged(const SessionSelection& now) = 0;
        /** Whose devices to show changed (the selected track, or a return or the master). */
        virtual void deviceOwnerChanged(const DeviceOwner&) {}
    };

    const SessionSelection& get() const noexcept { return current; }
    /** Changes the selection (no-op if it's the same) and tells the listeners. The Device view
        follows its track (a scene's selection keeps what it showed). */
    void set(const SessionSelection& s);

    const DeviceOwner& deviceOwner() const noexcept { return owner; }
    /** Shows a return's or the master's devices (or a track's) without changing the selection. */
    void showDevicesOf(const DeviceOwner& o);

    void addListener(Listener* l) { listeners.add(l); }
    void removeListener(Listener* l) { listeners.remove(l); }

private:
    SessionSelection current;
    DeviceOwner owner;
    juce::ListenerList<Listener> listeners;
};

inline void SessionSelectionModel::set(const SessionSelection& s)
{
    const bool changed = !(s == current);
    current = s;
    if (changed)
        listeners.call([this](Listener& l) { l.selectionChanged(current); });
    if (s.kind != SessionSelection::Kind::scene)
        showDevicesOf({DeviceOwner::Kind::track, s.track});
}

inline void SessionSelectionModel::showDevicesOf(const DeviceOwner& o)
{
    if (o == owner)
        return;
    owner = o;
    listeners.call([this](Listener& l) { l.deviceOwnerChanged(owner); });
}

} // namespace lattice::ui
