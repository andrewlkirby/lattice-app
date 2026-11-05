#include "control/modes/NoteMode.h"

#include "control/modes/SelectedClip.h"
#include "core/model/LiveSet.h"
#include "devices/racks/Rack.h"
#include "devices/racks/RackEditing.h"
#include "ui/session/KeyboardFollow.h"

#include <algorithm>

namespace lattice::control
{

namespace
{
std::uint32_t colourOf(const juce::ValueTree& node, std::uint32_t fallback)
{
    const auto argb = static_cast<std::uint32_t>(static_cast<juce::int64>(node.getProperty(ids::colour, 0)));
    return (argb >> 24) == 0 ? fallback : rgbOf(argb);
}

constexpr float trackDim = 0.35f;
constexpr float menuDim = 0.3f;
constexpr int stripRowsPerLength = 8; // Scroll: a row for each eighth of the strip

const juce::String dot = juce::String(juce::CharPointer_UTF8(" \xc2\xb7 ")); // U+00B7 middle dot

juce::String hzText(double hz)
{
    return hz > 0.0 ? juce::String(hz, hz < 100.0 ? 2 : 1) + " Hz" : juce::String("unmapped");
}
} // namespace

NoteMode::NoteMode(ModeHost& h) : Mode(h)
{
}

// --- The track and its layout -----------------------------------------------------------------------

int NoteMode::track() const
{
    auto& c = host.context();
    const auto& owner = c.selection.deviceOwner();
    if (owner.kind != ui::DeviceOwner::Kind::track)
        return -1;
    const int t = owner.index;
    if (t < 0 || t >= c.session.set().numTracks() || c.session.set().isGroupTrack(t))
        return -1;
    return t;
}

TuningPtr NoteMode::tuningOf(int trackIndex) const
{
    auto& c = host.context();
    auto& registry = c.session.tunings();
    if (trackIndex < 0)
        return registry.defaultTuning();
    auto tuning = registry.findById(c.session.set().track(trackIndex)[ids::tuningId].toString());
    return tuning != nullptr ? tuning : registry.defaultTuning();
}

ui::TuningRowContext NoteMode::rowContext(int trackIndex) const
{
    ui::TuningRowContext rc;
    shownTuning = tuningOf(trackIndex);
    rc.tuning = shownTuning.get();
    if (trackIndex < 0)
        return rc;
    auto& session = host.context().session;
    const auto node = session.set().track(trackIndex);
    rc.noteOffset = node[ids::noteOffset];
    rc.policy = static_cast<HighNotePolicy>(static_cast<int>(node[ids::highNotePolicy]));
    if (const auto processor = session.processorForTrack(trackIndex); processor != nullptr && processor->tuning().getSampleRate() > 0.0)
        rc.sampleRate = processor->tuning().getSampleRate();
    rc.highlight = LiveSet::highlightOf(node);
    return rc;
}

ui::TuningRow NoteMode::rowFor(int note) const
{
    return ui::tuningRow(rowContext(track()), note);
}

std::uint32_t NoteMode::trackColour(int trackIndex) const
{
    if (trackIndex < 0)
        return host.colours().solo;
    return colourOf(host.context().session.set().track(trackIndex), host.colours().solo);
}

NoteLayout NoteMode::layout() const
{
    const int t = track();
    const auto tuning = tuningOf(t);
    const auto fallback = NoteLayout::defaultFor(*tuning);
    if (t < 0)
        return fallback;
    const auto saved = host.context().session.set().track(t)[ids::noteLayout].toString();
    // Saved for another tuning (the track's changed since): that tuning's default instead.
    if (saved.isEmpty() || saved.fromFirstOccurrenceOf("tuning=", false, false).upToFirstOccurrenceOf(";", false, false) != tuning->id)
        return fallback;
    auto l = NoteLayout::fromString(saved, fallback);
    const auto kinds = NoteLayout::kindsFor(*tuning);
    return std::find(kinds.begin(), kinds.end(), l.kind) != kinds.end() ? l : fallback;
}

void NoteMode::setLayout(const NoteLayout& l)
{
    const int t = track();
    if (t < 0)
        return;
    const auto tuning = tuningOf(t);
    auto node = host.context().session.set().track(t);
    node.setProperty(ids::noteLayout, "tuning=" + tuning->id + ";" + l.toString(), nullptr);
}

int NoteMode::noteAtPad(int row, int column) const
{
    if (track() < 0)
        return -1;
    return layout().noteAt(row, column, caps().rows);
}

// --- The step sequencers -----------------------------------------------------------------------------

juce::ValueTree NoteMode::drumRack() const
{
    const int t = track();
    if (t < 0)
        return {};
    for (const auto& device : LiveSet::chainOf(host.context().session.set().track(t)))
        if (device[ids::deviceType].toString() == RackDevice::drumRackId)
            return device;
    return {};
}

NoteMode::Pads NoteMode::pads() const
{
    if (track() < 0)
        return Pads::play;
    if (drumRack().isValid())
        return Pads::drum;
    return layout().sequencer ? Pads::sequencer : Pads::play;
}

SequencerZones NoteMode::zones() const
{
    switch (pads())
    {
    case Pads::drum: return SequencerZones::drum(caps().rows, caps().columns, layout().drumPads);
    case Pads::sequencer: return SequencerZones::melodic(caps().rows, caps().columns);
    case Pads::play: break;
    }
    return {};
}

int NoteMode::drumStart() const
{
    const int start = ui::drumPadPageStart(host.context().session.set(), track());
    return start >= 0 ? start : 36;
}

int NoteMode::drumNoteAtPad(int row, int column) const
{
    const auto area = zones().pads;
    if (!area.contains(row, column))
        return -1;
    const int fromBottom = area.row + area.rows - 1 - row;
    const int note = drumStart() + fromBottom * area.columns + (column - area.column);
    return note >= 0 && note <= 127 ? note : -1;
}

int NoteMode::drumNote() const
{
    const int t = track();
    if (t < 0)
        return -1;
    if (const auto it = drumNotes.find(host.context().session.set().trackId(t)); it != drumNotes.end())
        return it->second;
    // None played yet: the first pad on the grid with something on it.
    const auto rack = drumRack();
    const auto area = zones().pads;
    const int start = drumStart();
    for (int i = 0; i < std::max(16, area.count()); ++i)
        if (start + i <= 127 && racks::padFor(rack, start + i).isValid())
            return start + i;
    return start;
}

juce::ValueTree NoteMode::sequencedClip() const
{
    return selectedClip(host.context(), track());
}

StepSequencer::Target NoteMode::sequencerTarget() const
{
    StepSequencer::Target target;
    const int t = track();
    if (t < 0)
        return target;
    auto& c = host.context();
    const auto l = layout();
    target.track = t;
    // The selected slot; with none (a track selected), the track's first empty one.
    const auto& set = c.session.set();
    target.scene = c.selection.get().scene;
    if (target.scene < 0 || target.scene >= set.numScenes())
    {
        target.scene = -1;
        for (int s = 0; s < set.numScenes() && target.scene < 0; ++s)
            if (!set.clip(t, s).isValid())
                target.scene = s;
    }
    target.clip = sequencedClip();
    target.stepLength = l.stepLength();
    target.colour = trackColour(t);
    if (pads() == Pads::drum)
    {
        target.pitches = {drumNote()};
        target.oneRow = true;
        return target;
    }
    const auto rc = rowContext(t);
    for (int k = 0; k < zones().steps.rows; ++k)
    {
        const int note = l.sequencerNote(k);
        target.pitches.push_back(note);
        const auto row = note >= 0 ? ui::tuningRow(rc, note) : ui::TuningRow{};
        target.tinted.push_back(note >= 0 && (l.kind == NoteLayout::Kind::table ? row.anchor : l.isRoot(note)));
        target.playable.push_back(note >= 0 && (!row.unplayable || row.foldsDown));
    }
    return target;
}

std::uint32_t NoteMode::aspects() const
{
    // Play and Record; the sequencers' playhead too.
    return ui::EngineStateFeed::transportAspect | (pads() != Pads::play ? ui::EngineStateFeed::slotsAspect : 0u);
}

void NoteMode::selectionChanged()
{
    steps.reset();
}

void NoteMode::encoderTargets(std::vector<juce::String>& targets)
{
    if (!ownsEncoders())
        return;
    steps.setZones(zones());
    steps.encoderTargets(sequencerTarget(), targets);
}

void NoteMode::display(DisplayFrame& frame)
{
    if (!ownsEncoders())
        return;
    steps.setZones(zones());
    steps.display(sequencerTarget(), frame);
}

void NoteMode::moveDrum(int direction)
{
    auto rack = drumRack();
    if (!rack.isValid())
        return;
    const auto area = zones().pads;
    const int count = std::max(1, area.count());
    const int by = host.shiftHeld() ? std::max(1, area.columns) : count;
    const int start = juce::jlimit(0, std::max(0, 128 - count), drumStart() + direction * by);
    rack.setProperty(ids::padStart, start, nullptr); // the view's state, as the Device view keeps it
    host.requestRefresh();
}

void NoteMode::moveSequencer(int rows)
{
    setLayout(layout().sequencerMoved(rows, zones().steps.rows));
    host.requestRefresh();
}

void NoteMode::pressLayout()
{
    const int t = track();
    if (t < 0)
        return;
    auto l = layout();
    steps.reset();
    if (pads() == Pads::drum)
    {
        l.drumPads = !l.drumPads;
        setLayout(l);
        return;
    }
    // The tuning's layouts, then the sequencer (in the first of them).
    const auto tuning = tuningOf(t);
    const auto kinds = NoteLayout::kindsFor(*tuning);
    if (l.sequencer)
    {
        l.sequencer = false;
        l = l.withKind(kinds.front(), *tuning);
    }
    else
    {
        const auto at = static_cast<std::size_t>(std::find(kinds.begin(), kinds.end(), l.kind) - kinds.begin());
        if (at + 1 < kinds.size())
            l = l.withKind(kinds[at + 1], *tuning);
        else
        {
            l = l.withKind(kinds.front(), *tuning);
            l.sequencer = true;
        }
    }
    setLayout(l);
}

// --- Input ------------------------------------------------------------------------------------------

bool NoteMode::handle(const SurfaceInput& input)
{
    using Kind = SurfaceInput::Kind;
    const int columns = caps().columns;
    switch (input.kind)
    {
    case Kind::padDown:
    {
        if (menu)
        {
            pressMenu(input.row, input.column);
            return true;
        }
        const auto p = pads();
        const int index = input.row * columns + input.column;
        int note = -1;
        if (p == Pads::play)
            note = noteAtPad(input.row, input.column);
        else if (p == Pads::drum && zones().pads.contains(input.row, input.column))
        {
            note = drumNoteAtPad(input.row, input.column);
            if (note >= 0)
                drumNotes[host.context().session.set().trackId(track())] = note; // its steps are shown
        }
        else
        {
            steps.setZones(zones());
            steps.press(sequencerTarget(), input.row, input.column);
            return true;
        }
        if (note >= 0 && !padNotes.contains(index))
        {
            ++pressedNotes[static_cast<std::size_t>(note)];
            padNotes[index] = note;
        }
        return true;
    }
    case Kind::padUp:
    {
        const int index = input.row * columns + input.column;
        if (const auto it = padNotes.find(index); it != padNotes.end())
        {
            auto& count = pressedNotes[static_cast<std::size_t>(it->second)];
            count = std::max(0, count - 1);
            padNotes.erase(it);
        }
        else if (!menu && pads() != Pads::play)
        {
            steps.setZones(zones());
            steps.release(sequencerTarget(), input.row, input.column);
        }
        return true;
    }
    case Kind::padPressure:
        return true;
    case Kind::strip:
        if (host.expression().strip == Expression::Strip::scroll && !menu)
        {
            if (stripLast >= 0.0f)
            {
                const int rows = static_cast<int>((input.value - stripLast) * stripRowsPerLength);
                if (rows != 0)
                {
                    setLayout(layout().scrolled(rows, caps().rows, columns));
                    stripLast += static_cast<float>(rows) / stripRowsPerLength;
                }
            }
            else
            {
                stripLast = input.value;
            }
        }
        return true;
    case Kind::stripRelease:
        stripLast = -1.0f;
        return true;
    case Kind::buttonDown:
    {
        const auto id = input.button;
        switch (id.role)
        {
        case Role::octaveUp: moveGrid(1); return true;
        case Role::octaveDown: moveGrid(-1); return true;
        case Role::navUp:
            if (menu)
                menuScroll = std::max(0, menuScroll - 1);
            else if (pads() == Pads::sequencer)
                moveSequencer(1);
            else
                moveGrid(1);
            host.requestRefresh();
            return true;
        case Role::navDown:
            if (menu)
            {
                const auto settings = menuSettings();
                menuScroll = std::min(menuScroll + 1, std::max(0, menuRowsNeeded(settings) - caps().rows));
            }
            else if (pads() == Pads::sequencer)
            {
                moveSequencer(-1);
            }
            else
            {
                moveGrid(-1);
            }
            host.requestRefresh();
            return true;
        case Role::navLeft: stepTrack(-1); return true;
        case Role::navRight: stepTrack(1); return true;
        case Role::scale: setMenuShown(!menu); return true;
        case Role::layout: pressLayout(); return true;
        case Role::trackSelect:
            if (const int t = host.trackOfColumn(id.index); t >= 0)
                selectTrack(t);
            return true;
        case Role::displayLower:
            if (!caps().has(ButtonId{Role::trackSelect, 0}))
                if (const int t = host.trackOfColumn(id.index); t >= 0)
                    selectTrack(t);
            return true;
        default: return false;
        }
    }
    default:
        return false;
    }
}

void NoteMode::moveGrid(int direction)
{
    if (track() < 0)
        return;
    if (pads() == Pads::drum)
    {
        moveDrum(direction);
        return;
    }
    auto l = layout();
    if (l.sequencer)
    {
        moveSequencer(host.shiftHeld() ? direction : direction * l.sequencerOctave(zones().steps.rows));
        return;
    }
    if (host.shiftHeld())
    {
        // One note: a table entry or a semitone (In Key: the root, so the scale moves with it).
        if (l.kind == NoteLayout::Kind::inKey)
            l = l.withRoot(l.root + direction);
        else
        {
            auto next = l;
            next.origin += direction;
            bool any = false; // a pad still plays
            for (int c = 0; c < caps().columns && !any; ++c)
                for (int r = 0; r < caps().rows && !any; ++r)
                    any = next.noteAt(r, c, caps().rows) >= 0;
            if (any)
                l = next;
        }
    }
    else
    {
        l = l.shifted(direction, caps().rows, caps().columns);
    }
    setLayout(l);
}

void NoteMode::selectTrack(int trackIndex)
{
    if (host.followsSelection())
        host.context().selection.set({ui::SessionSelection::Kind::track, trackIndex, host.context().selection.get().scene});
    host.requestRefresh();
}

void NoteMode::stepTrack(int direction)
{
    const auto& set = host.context().session.set();
    for (int t = track() + direction; t >= 0 && t < set.numTracks(); t += direction)
        if (!set.isGroupTrack(t))
        {
            selectTrack(t);
            return;
        }
}

void NoteMode::enter()
{
    pressedNotes.fill(0);
    padNotes.clear();
    menu = false;
    steps.reset();
}

void NoteMode::leave()
{
    pressedNotes.fill(0);
    padNotes.clear();
    menu = false;
    stripLast = -1.0f;
    steps.reset();
}

void NoteMode::setMenuShown(bool shown)
{
    menu = shown;
    menuScroll = 0;
    touched.clear();
    host.requestRefresh();
}

// --- The fast path ----------------------------------------------------------------------------------

void NoteMode::padMap(PadNoteMap& map)
{
    if (menu || track() < 0)
        return; // the menu's pads, or nothing to play: the message thread's
    if (pads() == Pads::drum)
    {
        // The drum pads play; the steps and the loop selector are the message thread's.
        for (int r = 0; r < map.rows; ++r)
            for (int c = 0; c < map.columns; ++c)
                if (const int note = drumNoteAtPad(r, c); note >= 0)
                {
                    auto& pad = map.pad(r, c);
                    pad.kind = PadNoteMap::Pad::Kind::note;
                    pad.note = static_cast<std::uint8_t>(note);
                }
        return;
    }
    if (pads() == Pads::sequencer)
        return;
    const auto l = layout();
    const auto rc = rowContext(track());
    for (int r = 0; r < map.rows; ++r)
        for (int c = 0; c < map.columns; ++c)
            if (const int note = l.noteAt(r, c, map.rows); note >= 0)
            {
                auto& pad = map.pad(r, c);
                pad.kind = PadNoteMap::Pad::Kind::note;
                pad.note = static_cast<std::uint8_t>(note);
            }
}

// --- Lights and words -------------------------------------------------------------------------------

void NoteMode::render(LedFrame& frame)
{
    const auto& colours = host.colours();
    const int rows = caps().rows, columns = caps().columns;
    const int t = track();
    const auto lit = LedState::solid(colours.white);
    const auto available = LedState::solid(dimmed(colours.white, colours.dim));

    const auto p = pads();
    if (menu)
    {
        renderMenu(frame);
    }
    else if (p == Pads::drum)
    {
        renderDrum(frame, t);
    }
    else if (p == Pads::sequencer)
    {
        steps.setZones(zones());
        steps.render(sequencerTarget(), frame);
    }
    else if (t >= 0)
    {
        const auto l = layout();
        const auto rc = rowContext(t);
        const auto own = trackColour(t);
        for (int r = 0; r < rows; ++r)
            for (int c = 0; c < columns; ++c)
            {
                const int note = l.noteAt(r, c, rows);
                if (note < 0)
                    continue;
                const auto row = ui::tuningRow(rc, note);
                const bool pressed = pressedNotes[static_cast<std::size_t>(note)] > 0;
                const bool playable = !row.unplayable || row.foldsDown;
                auto& led = frame.pad(r, c, columns);
                if (pressed)
                    led = LedState::solid(playable ? colours.play : colours.record);
                else if (!playable)
                    led = LedState::off();
                else if (l.kind == NoteLayout::Kind::table)
                    led = LedState::solid(row.anchor ? own : row.highlighted ? colours.white : dimmed(colours.white, colours.padDim));
                else if (l.isRoot(note))
                    led = LedState::solid(own);
                else
                    led = LedState::solid(l.inScale(note) ? colours.white : dimmed(colours.white, colours.padDim));
            }
    }

    // Buttons.
    const auto l = layout();
    bool canDown = t >= 0 && !menu && l.canShift(-1, rows, columns);
    bool canUp = t >= 0 && !menu && l.canShift(1, rows, columns);
    if (p == Pads::drum)
    {
        canDown = !menu && drumStart() > 0;
        canUp = !menu && drumStart() + zones().pads.count() < 128;
    }
    else if (p == Pads::sequencer)
    {
        const int shownRows = zones().steps.rows;
        canDown = !menu && !(l.sequencerMoved(-1, shownRows) == l);
        canUp = !menu && !(l.sequencerMoved(1, shownRows) == l);
    }
    setButton(frame, {Role::octaveUp, 0}, host.isHeld(Role::octaveUp) ? lit : canUp ? available : LedState::off());
    setButton(frame, {Role::octaveDown, 0}, host.isHeld(Role::octaveDown) ? lit : canDown ? available : LedState::off());
    setButton(frame, {Role::navUp, 0}, menu ? (menuScroll > 0 ? available : LedState::off()) : canUp ? available : LedState::off());
    if (menu)
    {
        const auto settings = menuSettings();
        setButton(frame, {Role::navDown, 0}, menuScroll + rows < menuRowsNeeded(settings) ? available : LedState::off());
    }
    else
    {
        setButton(frame, {Role::navDown, 0}, canDown ? available : LedState::off());
    }
    setButton(frame, {Role::navLeft, 0}, available);
    setButton(frame, {Role::navRight, 0}, available);
    setButton(frame, {Role::scale, 0}, menu ? lit : available);
    setButton(frame, {Role::layout, 0}, t >= 0 ? available : LedState::off());

    const Role trackRole = caps().has(ButtonId{Role::trackSelect, 0}) ? Role::trackSelect : Role::displayLower;
    for (int i = 0; i < caps().countOf(trackRole); ++i)
    {
        const int tr = host.trackOfColumn(i);
        if (tr < 0)
            continue;
        const auto colour = trackColour(tr);
        setButton(frame, {trackRole, i}, LedState::solid(tr == t ? colour : dimmed(colour, trackDim)));
    }
}

void NoteMode::renderDrum(LedFrame& frame, int trackIndex)
{
    const auto& colours = host.colours();
    const int columns = caps().columns;
    const auto z = zones();
    const auto rack = drumRack();
    const int selected = drumNote();
    const auto own = trackColour(trackIndex);
    for (int r = z.pads.row; r < z.pads.row + z.pads.rows; ++r)
        for (int c = z.pads.column; c < z.pads.column + z.pads.columns; ++c)
        {
            const int note = drumNoteAtPad(r, c);
            if (note < 0)
                continue;
            const auto pad = racks::padFor(rack, note);
            LedState led = LedState::solid(dimmed(colours.white, colours.padDim * 0.5f));
            if (pressedNotes[static_cast<std::size_t>(note)] > 0)
                led = LedState::solid(colours.play);
            else if (note == selected && !z.steps.empty())
                led = LedState::solid(colours.white);
            else if (pad.isValid())
                led = LedState::solid(colourOf(pad, own));
            frame.pad(r, c, columns) = led;
        }
    steps.setZones(z);
    steps.render(sequencerTarget(), frame);
}

void NoteMode::describe(SurfaceText& text)
{
    const int rows = caps().rows, columns = caps().columns;
    const int t = track();
    text.pads.assign(static_cast<std::size_t>(rows * columns), {});
    if (menu)
    {
        const auto settings = menuSettings();
        const auto at = menuPads(settings);
        for (std::size_t i = 0; i < at.size(); ++i)
            if (at[i].first >= 0)
                text.pads[i] = settings[static_cast<std::size_t>(at[i].first)].choices[static_cast<std::size_t>(at[i].second)].name;
        juce::StringArray summary;
        for (const auto& s : settings)
            for (const auto& c : s.choices)
                if (c.selected)
                    summary.add(s.name + ": " + c.name);
        text.status = "Scale menu" + dot + (touched.isNotEmpty() ? touched : summary.joinIntoString(dot));
        return;
    }
    if (t < 0)
    {
        text.status = "Note mode" + dot + "select a MIDI track";
        return;
    }

    const auto l = layout();
    const auto rc = rowContext(t);
    const auto& set = host.context().session.set();
    if (const auto p = pads(); p != Pads::play)
    {
        const auto z = zones();
        const auto target = sequencerTarget();
        steps.setZones(z);
        const auto rate = NoteLayout::stepRates()[static_cast<std::size_t>(std::clamp(l.stepRate, 0, static_cast<int>(NoteLayout::stepRates().size()) - 1))].first;
        const auto where = juce::String("page ") + juce::String(steps.page(target) + 1) + "/" + juce::String(steps.pages(target));
        const auto clipText = target.clip.isValid() ? where : juce::String("no clip: a step makes one");
        if (p == Pads::drum)
        {
            const auto rack = drumRack();
            for (int r = 0; r < rows; ++r)
                for (int c = 0; c < columns; ++c)
                    if (const int note = drumNoteAtPad(r, c); note >= 0)
                    {
                        const auto pad = racks::padFor(rack, note);
                        text.pads[static_cast<std::size_t>(r * columns + c)] = pad.isValid() && pad[ids::name].toString().isNotEmpty()
                                                                                   ? pad[ids::name].toString()
                                                                                   : juce::String(note);
                    }
            const auto pad = racks::padFor(rack, target.pitches.front());
            const auto padName = pad.isValid() && pad[ids::name].toString().isNotEmpty() ? pad[ids::name].toString()
                                                                                          : "pad " + juce::String(target.pitches.front());
            text.status = set.track(t)[ids::name].toString() + dot + "Drum" + dot + padName + dot + rate + dot + clipText;
            return;
        }
        // The melodic sequencer: each row's note on its first pad.
        for (int k = 0; k < z.steps.rows && k < static_cast<int>(target.pitches.size()); ++k)
            if (const int note = target.pitches[static_cast<std::size_t>(k)]; note >= 0)
            {
                const auto row = ui::tuningRow(rc, note);
                const int r = z.steps.row + z.steps.rows - 1 - k;
                text.pads[static_cast<std::size_t>(r * columns + z.steps.column)] =
                    row.namedNotes ? row.label : row.entry >= 0 ? juce::String(row.entry) : juce::String();
            }
        text.status = set.track(t)[ids::name].toString() + dot + rc.tuning->name + dot + "Sequencer (" + NoteLayout::kindName(l.kind) +
                      ")" + dot + rate + dot + clipText;
        return;
    }
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < columns; ++c)
            if (const int note = l.noteAt(r, c, rows); note >= 0)
            {
                const auto row = ui::tuningRow(rc, note);
                text.pads[static_cast<std::size_t>(r * columns + c)] =
                    row.namedNotes ? row.label : row.entry >= 0 ? juce::String(row.entry) : juce::String();
            }

    // What's pressed (lowest first), or what the grid is.
    juce::StringArray pressed;
    for (int note = 0; note < 128; ++note)
        if (pressedNotes[static_cast<std::size_t>(note)] > 0)
        {
            const auto row = ui::tuningRow(rc, note);
            auto s = (row.namedNotes ? row.label : "#" + juce::String(row.entry)) + " " + hzText(row.hz);
            if (row.anchor)
                s << " (anchor)";
            pressed.add(s);
        }
    if (!pressed.isEmpty())
    {
        text.status = pressed.joinIntoString(dot);
        return;
    }
    auto status = set.track(t)[ids::name].toString() + dot + rc.tuning->name + dot + NoteLayout::kindName(l.kind);
    if (l.kind == NoteLayout::Kind::table)
        status << dot << "from #" << (l.origin + rc.noteOffset) << ", rows +" << l.rowOffset;
    else
        status << dot << pitchClassName(l.root) << " " << scales()[static_cast<std::size_t>(l.scale)].name;
    if (!rc.highlight.empty())
        status << dot << static_cast<int>(rc.highlight.size()) << " highlighted";
    text.status = status;
}

// --- The Scale/Tuning menu --------------------------------------------------------------------------

std::vector<NoteMode::MenuSetting> NoteMode::menuSettings()
{
    std::vector<MenuSetting> settings;
    const int t = track();
    if (t < 0)
        return settings;
    const auto& colours = host.colours();
    auto& session = host.context().session;
    const auto tuning = tuningOf(t);
    const auto l = layout();
    const int columns = caps().columns, rows = caps().rows;

    MenuSetting rates{"Steps", colours.record, {}};
    for (int i = 0; i < static_cast<int>(NoteLayout::stepRates().size()); ++i)
        rates.choices.push_back({NoteLayout::stepRates()[static_cast<std::size_t>(i)].first, l.stepRate == i, [this, i] {
                                     auto next = layout();
                                     next.stepRate = i;
                                     setLayout(next);
                                 }});
    if (pads() == Pads::drum)
    {
        MenuSetting drumLayout{"Layout", colours.play, {}};
        for (const bool padsOnly : {false, true})
            drumLayout.choices.push_back({padsOnly ? "Pads" : "Pads and steps", l.drumPads == padsOnly, [this, padsOnly] {
                                              auto next = layout();
                                              next.drumPads = padsOnly;
                                              setLayout(next);
                                          }});
        settings.push_back(std::move(drumLayout));
        settings.push_back(std::move(rates));
        return settings;
    }

    MenuSetting tunings{"Tuning", colours.solo, {}};
    for (const auto& option : session.tunings().getAll())
    {
        const auto id = option->id;
        tunings.choices.push_back({option->name, option->id == tuning->id, [this, t, id] {
                                       auto& s = host.context().session;
                                       s.beginEdit("Change tuning", s.set().track(t));
                                       s.set().setTrackTuning(t, id);
                                   }});
    }
    settings.push_back(std::move(tunings));

    MenuSetting kinds{"Layout", colours.play, {}};
    for (const auto k : NoteLayout::kindsFor(*tuning))
        kinds.choices.push_back({NoteLayout::kindName(k), !l.sequencer && l.kind == k, [this, k, tuning] {
                                     auto next = layout().withKind(k, *tuning);
                                     next.sequencer = false;
                                     setLayout(next);
                                 }});
    kinds.choices.push_back({"Sequencer", l.sequencer, [this] {
                                 auto next = layout();
                                 next.sequencer = true;
                                 setLayout(next);
                             }});
    settings.push_back(std::move(kinds));
    if (l.sequencer)
        settings.push_back(std::move(rates));

    if (l.kind != NoteLayout::Kind::table)
    {
        MenuSetting roots{"Root", trackColour(t), {}};
        for (int pc = 0; pc < 12; ++pc)
            roots.choices.push_back({pitchClassName(pc), l.root == pc, [this, pc] { setLayout(layout().withRoot(pc)); }});
        settings.push_back(std::move(roots));

        MenuSetting scaleSetting{"Scale", colours.white, {}};
        for (int i = 0; i < static_cast<int>(scales().size()); ++i)
            scaleSetting.choices.push_back({scales()[static_cast<std::size_t>(i)].name, l.scale == i, [this, i] {
                                                auto next = layout();
                                                next.scale = i;
                                                setLayout(next);
                                            }});
        settings.push_back(std::move(scaleSetting));
    }

    MenuSetting rowSetting{"Rows", colours.mute, {}};
    for (const auto& [name, offset] : NoteLayout::rowPresets(l.kind, columns))
        rowSetting.choices.push_back({name, l.rowOffset == offset, [this, offset] {
                                          auto next = layout();
                                          next.rowOffset = offset;
                                          setLayout(next);
                                      }});
    for (const int delta : {-1, 1})
        rowSetting.choices.push_back({(delta < 0 ? "Rows -1 (" : "Rows +1 (") + juce::String(l.rowOffset) + ")", false, [this, delta] {
                                          auto next = layout();
                                          next.rowOffset = std::clamp(next.rowOffset + delta, 1, NoteLayout::maxRowOffset);
                                          setLayout(next);
                                      }});
    settings.push_back(std::move(rowSetting));

    if (l.kind == NoteLayout::Kind::table)
    {
        MenuSetting octave{"Octave buttons", colours.arm, {}};
        for (const auto step : {NoteLayout::OctaveStep::row, NoteLayout::OctaveStep::page})
            octave.choices.push_back({step == NoteLayout::OctaveStep::row ? "By a row" : "By a page", l.octaveStep == step, [this, step] {
                                          auto next = layout();
                                          next.octaveStep = step;
                                          setLayout(next);
                                      }});
        settings.push_back(std::move(octave));
    }
    juce::ignoreUnused(rows);
    return settings;
}

int NoteMode::menuRowsNeeded(const std::vector<MenuSetting>& settings) const
{
    const int columns = std::max(1, caps().columns);
    int needed = 0;
    for (const auto& s : settings)
        needed += std::max(1, (static_cast<int>(s.choices.size()) + columns - 1) / columns);
    return needed;
}

std::vector<std::pair<int, int>> NoteMode::menuPads(const std::vector<MenuSetting>& settings) const
{
    const int rows = caps().rows, columns = std::max(1, caps().columns);
    std::vector<std::pair<int, int>> at(static_cast<std::size_t>(rows * columns), {-1, -1});
    int gridRow = -menuScroll;
    for (std::size_t s = 0; s < settings.size(); ++s)
    {
        const int count = static_cast<int>(settings[s].choices.size());
        for (int i = 0; i < count; ++i)
        {
            const int r = gridRow + i / columns, c = i % columns;
            if (r >= 0 && r < rows)
                at[static_cast<std::size_t>(r * columns + c)] = {static_cast<int>(s), i};
        }
        gridRow += std::max(1, (count + columns - 1) / columns); // each setting starts a new row
    }
    return at;
}

void NoteMode::pressMenu(int row, int column)
{
    auto settings = menuSettings();
    const auto at = menuPads(settings);
    const auto index = static_cast<std::size_t>(row * caps().columns + column);
    if (index >= at.size() || at[index].first < 0)
        return;
    const auto& setting = settings[static_cast<std::size_t>(at[index].first)];
    const auto& choice = setting.choices[static_cast<std::size_t>(at[index].second)];
    touched = setting.name + ": " + choice.name;
    if (choice.choose)
        choice.choose();
    host.requestRefresh();
}

void NoteMode::renderMenu(LedFrame& frame)
{
    const auto settings = menuSettings();
    const auto at = menuPads(settings);
    for (std::size_t i = 0; i < at.size() && i < frame.pads.size(); ++i)
    {
        if (at[i].first < 0)
            continue;
        const auto& setting = settings[static_cast<std::size_t>(at[i].first)];
        const bool selected = setting.choices[static_cast<std::size_t>(at[i].second)].selected;
        frame.pads[i] = LedState::solid(selected ? host.colours().white : dimmed(setting.colour, menuDim));
    }
}

} // namespace lattice::control
