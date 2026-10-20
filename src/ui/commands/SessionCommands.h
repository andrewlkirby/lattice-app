#pragma once

#include "engine/SessionController.h"
#include "ui/commands/CommandRegistry.h"
#include "ui/session/SessionSelection.h"

#include <functional>

namespace lattice::ui
{

/** What only the window can do for these commands: dialogs and prompts. A missing function leaves
    its command disabled. */
struct ShellActions
{
    std::function<void()> newSet;     // asks to save changes first
    std::function<void()> openSet;    // a file dialog
    std::function<void()> saveSet;    // Save As if the set has no file yet
    std::function<void()> saveSetAs;
    std::function<void()> importMidi; // a file dialog, into the selected slot
    std::function<void()> rename;     // a prompt for the selected clip, scene or track
};

/** Where the commands read and change the selection. */
struct SelectionAccess
{
    std::function<SessionSelection()> get;
    std::function<void(SessionSelection)> set;
};

/** The transport, file, edit, create and Session commands: every action the M2/M3 debug UI has,
    as a command (docs/05-m4-ui.md, 4a). The default shortcuts follow Live where Live has one, and
    leave the computer keyboard's letters alone (A-L row, Z X C V M). */
void addSessionCommands(CommandRegistry& registry, SessionController& session, SelectionAccess selection,
                        ShellActions shell);

} // namespace lattice::ui
