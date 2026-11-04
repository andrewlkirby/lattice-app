#include "control/surface/SurfaceController.h"

#include "control/modes/DeviceMode.h"
#include "control/modes/MixMode.h"
#include "control/modes/NoteMode.h"
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

bool isEncoderMode(ModeKind kind)
{
    return kind == ModeKind::device || kind == ModeKind::mix;
}
} // namespace

SurfaceController::SurfaceController(SurfaceContext context, SurfaceDriver& d) : ctx(context), driver(d)
{
    if (ctx.specs == nullptr)
        ownSpecs = std::make_unique<DeviceSpecs>(ctx.session.deviceRegistry());
    deviceSpecs = ctx.specs != nullptr ? ctx.specs : ownSpecs.get();
    bank = std::make_unique<EncoderBank>(ctx.session.set(), *deviceSpecs, &ctx.session.undoManager());
    modes.resize(static_cast<std::size_t>(ModeKind::clip) + 1);
    auto s = std::make_unique<SessionMode>(*this);
    session = s.get();
    modes[static_cast<std::size_t>(ModeKind::session)] = std::move(s);
    auto n = std::make_unique<NoteMode>(*this);
    note = n.get();
    modes[static_cast<std::size_t>(ModeKind::note)] = std::move(n);
    auto dm = std::make_unique<DeviceMode>(*this);
    device = dm.get();
    modes[static_cast<std::size_t>(ModeKind::device)] = std::move(dm);
    auto mm = std::make_unique<MixMode>(*this);
    mix = mm.get();
    modes[static_cast<std::size_t>(ModeKind::mix)] = std::move(mm);
    active = static_cast<std::size_t>(ModeKind::session);
    // A device with encoders starts with them on its devices, as a Push does.
    if (driver.capabilities().encoders > 0)
        knob = static_cast<std::size_t>(ModeKind::device);

    auto& engine = ctx.session.audioEngine();
    source = engine.midiSourceFor("lattice.surface." + driver.capabilities().name);
    player = std::make_unique<PadPlayer>(engine, driver.capabilities());
    published = std::make_shared<const PadNoteMap>(*player->published());
    driver.pads = player.get();
    driver.onInput = [this](const SurfaceInput& input) { handle(input); };
    ctx.selection.addListener(this);
    ctx.session.set().state().addListener(this);
    listenToFeed();
    current().enter();
    if (auto* k = knobs())
        k->enter();
    refresh();
}

SurfaceController::~SurfaceController()
{
    cancelPendingUpdate();
    bank->endGestures();
    driver.onInput = nullptr;
    driver.pads = nullptr;
    player.reset(); // ends what the pads hold
    ctx.feed.removeListener(this);
    ctx.session.set().state().removeListener(this);
    ctx.selection.removeListener(this);
}

ModeKind SurfaceController::modeKind() const noexcept
{
    return static_cast<ModeKind>(active);
}

std::optional<ModeKind> SurfaceController::encoderModeKind() const noexcept
{
    return knob < modes.size() ? std::optional<ModeKind>(static_cast<ModeKind>(knob)) : std::nullopt;
}

bool SurfaceController::isShown(ModeKind kind) const noexcept
{
    return static_cast<std::size_t>(kind) == active || static_cast<std::size_t>(kind) == knob;
}

bool SurfaceController::isAvailable(ModeKind kind) const noexcept
{
    const auto i = static_cast<std::size_t>(kind);
    return i < modes.size() && modes[i] != nullptr;
}

void SurfaceController::setMode(ModeKind kind)
{
    if (!isAvailable(kind))
        return;
    const auto index = static_cast<std::size_t>(kind);
    if (isEncoderMode(kind) && capabilities().encoders > 0)
    {
        if (index == knob)
            return;
        bank->endGestures();
        if (auto* k = knobs())
            k->leave();
        knob = index;
    }
    else
    {
        if (index == active)
            return;
        if (isEncoderMode(current().kind()))
            bank->endGestures(); // its faders
        current().leave();
        active = index;
    }
    listenToFeed();
    modes[index]->enter();
    refresh();
}

void SurfaceController::setFollowsSelection(bool f)
{
    follow = f;
    if (follow)
        selectionChanged(ctx.selection.get());
    refresh();
}

void SurfaceController::setExpression(const Expression& e)
{
    expressionSettings = e;
    refresh();
}

int SurfaceController::trackOfColumn(int column) const
{
    return session->trackOfColumn(column);
}

std::vector<int> SurfaceController::tracksInView(int count) const
{
    const auto all = session->columnsOfSet();
    std::vector<int> out;
    for (int c = session->firstColumn(); c < static_cast<int>(all.size()) && static_cast<int>(out.size()) < count; ++c)
        out.push_back(all[static_cast<std::size_t>(c)]);
    return out;
}

void SurfaceController::scrollTracks(int columns)
{
    session->setOrigin(session->firstColumn() + columns, session->firstScene());
    requestRefresh();
}

void SurfaceController::pressTrack(int track)
{
    session->pressTrack(track);
}

LedState SurfaceController::trackLed(int track) const
{
    return session->trackLed(track);
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

    switch (input.kind)
    {
    case Kind::encoderTurn:
    case Kind::encoderSet:
    case Kind::encoderTouch:
    case Kind::encoderRelease:
        if (!encoderOwner().handle(input))
            handleEncoder(input);
        break;
    default:
        if (handleGlobal(input))
            break;
        {
            // The buttons along the display are the encoder mode's first; the rest the grid's.
            const bool alongDisplay = (input.kind == Kind::buttonDown || wasRelease) &&
                                      (input.button.role == Role::displayUpper || input.button.role == Role::displayLower);
            Mode* first = alongDisplay && knobs() != nullptr ? knobs() : &current();
            Mode* second = first == &current() ? knobs() : &current();
            if (!first->handle(input) && second != nullptr)
                second->handle(input);
        }
        break;
    }
    if (input.kind == Kind::buttonDown || wasRelease || input.kind == Kind::padDown || input.kind == Kind::padUp ||
        input.kind == Kind::encoderTouch || input.kind == Kind::encoderRelease)
        requestRefresh(); // what a held button, a pressed pad or a touched encoder shows
}

void SurfaceController::handleEncoder(const SurfaceInput& input)
{
    switch (input.kind)
    {
    case SurfaceInput::Kind::encoderTurn: bank->turn(input.encoder, input.delta, capabilities().encoderResolution, shiftHeld()); break;
    case SurfaceInput::Kind::encoderSet: bank->set(input.encoder, input.value); break;
    case SurfaceInput::Kind::encoderTouch: bank->touch(input.encoder); break;
    case SurfaceInput::Kind::encoderRelease: bank->release(input.encoder); break;
    default: break;
    }
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
        if (static_cast<std::size_t>(*mode) == knob)
            return true;
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
        set(role, isShown(mode) ? lit : isAvailable(mode) ? available : LedState::off());
    }
}

void SurfaceController::refresh()
{
    cancelPendingUpdate();
    publishPadMap();

    std::vector<juce::String> targets;
    encoderOwner().encoderTargets(targets);
    bank->assign(targets);

    if (capabilities().hasDisplay())
    {
        DisplayFrame frame;
        encoderOwner().display(frame);
        if (!shownAnyFrame || !(frame == shownFrame))
        {
            shownFrame = std::move(frame);
            shownAnyFrame = true;
            driver.showDisplay(shownFrame);
        }
    }

    SurfaceText text;
    current().describe(text);
    if (!shownAnyText || !(text == shownText))
    {
        shownText = std::move(text);
        shownAnyText = true;
        driver.showText(shownText);
    }

    auto frame = LedFrame::blank(capabilities());
    renderGlobals(frame);
    current().render(frame);
    if (auto* k = knobs())
        k->render(frame);
    auto changes = frame.changesFrom(shownAny ? shown : LedFrame{});
    if (changes.empty() && shownAny)
        return;
    shown = std::move(frame);
    shownAny = true;
    ++sent;
    driver.showLeds(shown, changes);
}

void SurfaceController::publishPadMap()
{
    auto map = PadNoteMap::empty(capabilities());
    map.source = source;
    map.setExpression(expressionSettings, capabilities());
    current().padMap(map);
    if (published != nullptr && map == *published)
        return;
    published = std::make_shared<const PadNoteMap>(std::move(map));
    player->publish(published);
}

void SurfaceController::listenToFeed()
{
    ctx.feed.removeListener(this);
    ctx.feed.addListener(this, current().aspects() | (knobs() != nullptr ? knobs()->aspects() : 0u));
}

void SurfaceController::engineStateChanged(std::uint32_t)
{
    refresh();
}

void SurfaceController::selectionChanged(const ui::SessionSelection&)
{
    if (follow)
    {
        current().selectionChanged();
        // The Session box follows the selection under any mode: Mix's tracks are the box's.
        if (session != &current())
            session->selectionChanged();
        if (auto* k = knobs())
            k->selectionChanged();
    }
    triggerAsyncUpdate();
}

} // namespace lattice::control
