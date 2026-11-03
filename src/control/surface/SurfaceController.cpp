#include "control/surface/SurfaceController.h"

#include "control/modes/SessionMode.h"

#include <algorithm>

namespace lattice::control
{

namespace
{
std::optional<ModeKind> modeOfButton(Role role)
{
    switch (role)
    {
    case Role::modeSession: return ModeKind::session;
    case Role::modeNote: return ModeKind::note;
    case Role::modeDevice: return ModeKind::device;
    case Role::modeMix: return ModeKind::mix;
    case Role::modeBrowse: return ModeKind::browse;
    case Role::modeClip: return ModeKind::clip;
    default: return std::nullopt;
    }
}
} // namespace

SurfaceController::SurfaceController(SurfaceContext context, SurfaceDriver& d) : ctx(context), driver(d)
{
    modes.resize(static_cast<std::size_t>(ModeKind::clip) + 1);
    auto s = std::make_unique<SessionMode>(*this);
    session = s.get();
    modes[static_cast<std::size_t>(ModeKind::session)] = std::move(s);
    active = static_cast<std::size_t>(ModeKind::session);

    driver.onInput = [this](const SurfaceInput& input) { handle(input); };
    ctx.selection.addListener(this);
    ctx.session.set().state().addListener(this);
    listenToFeed();
    current().enter();
    refresh();
}

SurfaceController::~SurfaceController()
{
    cancelPendingUpdate();
    driver.onInput = nullptr;
    ctx.feed.removeListener(this);
    ctx.session.set().state().removeListener(this);
    ctx.selection.removeListener(this);
}

ModeKind SurfaceController::modeKind() const noexcept
{
    return static_cast<ModeKind>(active);
}

bool SurfaceController::isAvailable(ModeKind kind) const noexcept
{
    const auto i = static_cast<std::size_t>(kind);
    return i < modes.size() && modes[i] != nullptr;
}

void SurfaceController::setMode(ModeKind kind)
{
    if (!isAvailable(kind) || static_cast<std::size_t>(kind) == active)
        return;
    current().leave();
    active = static_cast<std::size_t>(kind);
    listenToFeed();
    current().enter();
    refresh();
}

void SurfaceController::setFollowsSelection(bool f)
{
    follow = f;
    if (follow)
        current().selectionChanged();
    refresh();
}

void SurfaceController::setColours(const SurfaceColours& c)
{
    palette = c;
    refresh();
}

bool SurfaceController::isHeld(ButtonId id) const
{
    return std::find(held.begin(), held.end(), id) != held.end();
}

void SurfaceController::handle(const SurfaceInput& input)
{
    using Kind = SurfaceInput::Kind;
    if (input.kind == Kind::buttonDown && !isHeld(input.button))
        held.push_back(input.button);
    const bool wasRelease = input.kind == Kind::buttonUp;
    if (wasRelease)
        held.erase(std::remove(held.begin(), held.end(), input.button), held.end());

    if (!handleGlobal(input))
        current().handle(input);
    if (input.kind == Kind::buttonDown || wasRelease)
        requestRefresh(); // what a held button shows
}

bool SurfaceController::handleGlobal(const SurfaceInput& input)
{
    if (input.kind != SurfaceInput::Kind::buttonDown && input.kind != SurfaceInput::Kind::buttonUp)
        return false;
    const auto role = input.button.role;
    if (role == Role::shift)
        return true;
    if (input.kind == SurfaceInput::Kind::buttonUp)
        return modeOfButton(role).has_value(); // a mode reads what's held; releases are the controller's

    auto& commands = ctx.commands;
    const bool shift = shiftHeld();
    switch (role)
    {
    case Role::play: commands.perform(shift ? "transport.continue" : "transport.playStop"); return true;
    case Role::record: commands.perform("transport.sessionRecord"); return true;
    case Role::metronome: commands.perform("transport.metronome"); return true;
    case Role::tapTempo: commands.perform("transport.tapTempo"); return true;
    case Role::undo: commands.perform(shift ? "edit.redo" : "edit.undo"); return true;
    case Role::stopAll: commands.perform("transport.stopAllClips"); return true;
    case Role::newClip: commands.perform(shift ? "create.scene" : "create.midiClip"); return true;
    case Role::stopClip:
        // Shift+Stop Clip stops every clip (as on Push 2); held alone it's Session mode's modifier.
        if (shift)
        {
            commands.perform("transport.stopAllClips");
            return true;
        }
        return false;
    default: break;
    }
    if (const auto mode = modeOfButton(role))
    {
        if (static_cast<std::size_t>(*mode) == active)
            return role != Role::modeSession; // a held Session button shows the overview
        setMode(*mode);
        return true;
    }
    return false;
}

void SurfaceController::renderGlobals(LedFrame& frame) const
{
    const auto& caps = capabilities();
    const auto set = [&](Role role, LedState state) {
        if (const int i = caps.indexOf(ButtonId{role, 0}); i >= 0)
            frame.buttons[static_cast<std::size_t>(i)] = state;
    };
    const auto available = LedState::solid(dimmed(palette.white, palette.dim));
    const auto lit = LedState::solid(palette.white);
    const auto& transport = ctx.feed.transport();

    set(Role::play, transport.playing ? LedState::solid(palette.play) : available);
    set(Role::record, transport.sessionRecord ? LedState::solid(palette.record) : LedState::solid(dimmed(palette.record, palette.dim)));
    set(Role::metronome, ctx.session.set().metronomeOn() ? lit : available);
    set(Role::tapTempo, isHeld(Role::tapTempo) ? lit : available);
    set(Role::shift, shiftHeld() ? lit : available);
    set(Role::undo, isHeld(Role::undo) ? lit : ctx.session.undoManager().canUndo() ? available : LedState::off());
    set(Role::stopAll, isHeld(Role::stopAll) ? lit : available);
    set(Role::newClip, isHeld(Role::newClip) ? lit : available);
    for (const auto role : {Role::modeSession, Role::modeNote, Role::modeDevice, Role::modeMix, Role::modeBrowse, Role::modeClip})
    {
        const auto mode = *modeOfButton(role);
        set(role, static_cast<std::size_t>(mode) == active ? lit : isAvailable(mode) ? available : LedState::off());
    }
}

void SurfaceController::refresh()
{
    cancelPendingUpdate();
    auto frame = LedFrame::blank(capabilities());
    renderGlobals(frame);
    current().render(frame);
    auto changes = frame.changesFrom(shownAny ? shown : LedFrame{});
    if (changes.empty() && shownAny)
        return;
    shown = std::move(frame);
    shownAny = true;
    ++sent;
    driver.showLeds(shown, changes);
}

void SurfaceController::listenToFeed()
{
    ctx.feed.removeListener(this);
    ctx.feed.addListener(this, current().aspects());
}

void SurfaceController::engineStateChanged(std::uint32_t)
{
    refresh();
}

void SurfaceController::selectionChanged(const ui::SessionSelection&)
{
    if (follow)
        current().selectionChanged();
    triggerAsyncUpdate();
}

} // namespace lattice::control
