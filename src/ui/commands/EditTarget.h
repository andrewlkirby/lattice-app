#pragma once

#include <juce_core/juce_core.h>

namespace lattice::ui
{

/** A view that takes the Edit commands (Duplicate, Delete, Copy, Cut, Paste, Select All) while it
    has the keyboard focus, as in Live, where they act on whatever view you're working in: the
    piano roll's notes rather than the Session view's clips. The commands, their shortcuts and
    their menu items stay the same ones. */
struct EditTarget
{
    virtual ~EditTarget() = default;
    /** Whether it can do this Edit command now ("edit.duplicate"...). */
    virtual bool canEdit(const juce::String& commandId) const = 0;
    virtual void edit(const juce::String& commandId) = 0;
};

} // namespace lattice::ui
