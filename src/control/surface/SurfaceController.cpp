#include "control/surface/SurfaceController.h"

#include "control/modes/BrowseMode.h"
#include "control/modes/ClipMode.h"
#include "control/modes/DeviceMode.h"
#include "control/modes/MixMode.h"
#include "control/modes/NoteMode.h"
#include "control/modes/SelectedClip.h"
#include "control/modes/SessionMode.h"
#include "ui/clip/ClipActions.h"

#include <algorithm>
#include <cmath>

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
    return kind == ModeKind::device || kind == ModeKind::mix || kind == ModeKind::clip || kind == ModeKind::browse;
}
} // namespace

SurfaceController::SurfaceController(SurfaceContext context, SurfaceDriver& d)
    : ctx(context), own{ctx.session, ctx.commands, ownSelection, ctx.feed, ctx.specs, ctx.browser}, driver(d)
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
    auto cm = std::make_unique<ClipMode>(*this);
    clipModePtr = cm.get();
    modes[static_cast<std::size_t>(ModeKind::clip)] = std::move(cm);
    if (ctx.browser != nullptr)
    {
        auto bm = std::make_unique<BrowseMode>(*this, *ctx.browser);
        browseModePtr = bm.get();
        modes[static_cast<std::size_t>(ModeKind::browse)] = std::move(bm);
    }
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
    ownSelection.addListener(this);
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
    ownSelection.removeListener(this);
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
    if (f == follow)
        return;
    if (!f)
    {
        // Its own from here, starting where the shared one is.
        ownSelection.showDevicesOf(ctx.selection.deviceOwner());
        ownSelection.set(ctx.selection.get());
        ownSelection.selectDevice(ctx.selection.selectedDevice());
    }
    follow = f;
    showSelection();
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
    case Kind::tempoTurn: turnTempo(input.delta); break;
    case Kind::padSlide:
    case Kind::padTimbre: break; // the fast path's alone: nothing a mode shows
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

void SurfaceController::turnTempo(int steps)
{
    // A BPM a step (Shift: a hundredth); a turn is one undo step (a pause of a second ends it).
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (now - lastTempoTurn > 1000.0)
        ctx.session.beginEdit("Tempo");
    lastTempoTurn = now;
    auto& set = ctx.session.set();
    set.setTempo(std::round((set.tempo() + steps * (shiftHeld() ? 0.01 : 1.0)) * 100.0) / 100.0);
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
    case Role::newClip: createNew(shift); return true;
    case Role::doubleLoop: ui::clipActions::duplicateLoop(ctx.session, selectedClip(context(), selectedTrack(context()))); return true;
    case Role::quantize: ui::clipActions::quantize(ctx.session, selectedClip(context(), selectedTrack(context()))); return true;
    case Role::fixedLength:
    {
        // On and off (back to the length it had); with Shift, the next length.
        auto& set = ctx.session.set();
        const auto& lengths = ui::clipActions::fixedLengths();
        int bars = set.fixedRecordBars();
        if (shift)
        {
            const auto at = std::find(lengths.begin(), lengths.end(), bars);
            bars = at == lengths.end() || at + 1 == lengths.end() ? lengths[1] : *(at + 1);
        }
        else if (bars != 0)
        {
            fixedBefore = bars;
            bars = 0;
        }
        else
        {
            bars = fixedBefore;
        }
        ctx.session.beginEdit(bars == 0 ? "Fixed length off" : "Fixed length " + juce::String(bars) + (bars == 1 ? " bar" : " bars"));
        set.setFixedRecordBars(bars);
        return true;
    }
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
    set(Role::fixedLength, ctx.session.set().fixedRecordBars() != 0 ? lit : available);
    const auto& c = follow ? ctx : own;
    const bool hasClip = selectedClip(c, selectedTrack(c)).isValid();
    set(Role::doubleLoop, isHeld(Role::doubleLoop) ? lit : hasClip ? available : LedState::off());
    set(Role::quantize, isHeld(Role::quantize) ? lit : hasClip ? available : LedState::off());
    for (const auto role : {Role::modeSession, Role::modeNote, Role::modeDevice, Role::modeMix, Role::modeBrowse, Role::modeClip})
    {
        const auto mode = *modeOfButton(role);
        set(role, isShown(mode) ? lit : isAvailable(mode) ? available : LedState::off());
    }
}

void SurfaceController::refresh()
{
    const double began = juce::Time::getMillisecondCounterHiRes();
    cancelPendingUpdate();
    // What the modes show can change what they need from the feed (Note mode's sequencer).
    if ((current().aspects() | (knobs() != nullptr ? knobs()->aspects() : 0u)) != listened)
        listenToFeed();
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
    if (auto* k = knobs(); k != nullptr && &encoderOwner() == k)
        k->render(frame);
    auto changes = frame.changesFrom(shownAny ? shown : LedFrame{});
    if (!changes.empty() || !shownAny)
    {
        shown = std::move(frame);
        shownAny = true;
        ++sent;
        driver.showLeds(shown, changes);
    }
    const double done = juce::Time::getMillisecondCounterHiRes();
    costs.record(done - began);
    if (pendingSince >= 0.0)
    {
        response.record(done - pendingSince);
        pendingSince = -1.0;
    }
}

void SurfaceController::publishPadMap()
{
    auto map = PadNoteMap::empty(capabilities());
    map.source = source;
    map.setExpression(expressionSettings, capabilities());
    if (!follow)
    {
        // Its own track's: the notes go there, not to the armed tracks.
        const auto& o = ownSelection.deviceOwner();
        const auto& set = ctx.session.set();
        if (o.kind == ui::DeviceOwner::Kind::track && o.index >= 0 && o.index < set.numTracks())
            map.target = static_cast<std::uint32_t>(set.trackId(o.index));
    }
    current().padMap(map);
    if (published != nullptr && map == *published)
        return;
    published = std::make_shared<const PadNoteMap>(std::move(map));
    player->publish(published);
}

void SurfaceController::listenToFeed()
{
    ctx.feed.removeListener(this);
    listened = current().aspects() | (knobs() != nullptr ? knobs()->aspects() : 0u);
    ctx.feed.addListener(this, listened);
}

void SurfaceController::engineStateChanged(std::uint32_t)
{
    refresh();
}

void SurfaceController::selectionChanged(const ui::SessionSelection& now)
{
    if (&now == &selection().get()) // the one the modes use (not the other)
        showSelection();
    later();
}

void SurfaceController::showSelection()
{
    current().selectionChanged();
    // The Session box follows the selection under any mode: Mix's tracks are the box's.
    if (session != &current())
        session->selectionChanged();
    if (auto* k = knobs())
        k->selectionChanged();
}

void SurfaceController::createNew(bool scene)
{
    if (follow)
    {
        ctx.commands.perform(scene ? "create.scene" : "create.midiClip");
        return;
    }
    // The window's commands act on the shared selection: the same, on this surface's own.
    auto& set = ctx.session.set();
    const auto s = ownSelection.get();
    if (scene)
    {
        ctx.session.beginEdit("Insert scene");
        ownSelection.set({ui::SessionSelection::Kind::scene, s.track, set.addScene(s.scene + 1)});
    }
    else if (s.kind == ui::SessionSelection::Kind::slot && s.track >= 0 && s.track < set.numTracks() &&
             !set.clip(s.track, s.scene).isValid() && !set.isGroupTrack(s.track))
    {
        ctx.session.beginEdit("Insert MIDI clip", set.slot(s.track, s.scene));
        set.createClip(s.track, s.scene, set.beatsPerBar());
    }
}

} // namespace lattice::control
