#pragma once

#include "ui/browser/BrowserModel.h"

#include <juce_data_structures/juce_data_structures.h>

namespace lattice::control
{

/** What Browse mode drives (docs/06-m5-controllers.md, "Browse mode"): the window's browser, so a
    controller and the screen browse the same list, its selection plays the same previews on the
    M4 preview bus, and Load inserts where the Device view is looking, by the same rules.
    The shell implements it; a surface without one has no Browse mode. Message thread. */
class BrowseHost
{
public:
    virtual ~BrowseHost() = default;

    /** The browser's sources, rows and selection. Selecting a row previews it (with previews on). */
    virtual ui::BrowserModel& browser() = 0;
    /** Inserts the selected row (or, in hot-swap, replaces the device). False if nothing went in. */
    virtual bool load() = 0;

    virtual bool previewOn() const = 0;
    virtual void setPreviewOn(bool on) = 0;

    /** Hot-swap: the selected device is replaced by the next load (an invalid tree: off). */
    virtual juce::ValueTree hotSwapTarget() const = 0;
    /** Turns hot-swap on for the device the Device view has selected, or off. */
    virtual void setHotSwap(bool on) = 0;

    struct Listener
    {
        virtual ~Listener() = default;
        /** The rows, the selection or the source changed. */
        virtual void browserChanged() = 0;
    };
    void addListener(Listener* l) { listeners.add(l); }
    void removeListener(Listener* l) { listeners.remove(l); }

protected:
    void changed() { listeners.call([](Listener& l) { l.browserChanged(); }); }

private:
    juce::ListenerList<Listener> listeners;
};

} // namespace lattice::control
