#include "control/modes/NoteMode.h"

#include "core/model/LiveSet.h"

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
        const int note = noteAtPad(input.row, input.column);
        const int index = input.row * columns + input.column;
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
            else
            {
                moveGrid(-1);
            }
            host.requestRefresh();
            return true;
        case Role::navLeft: stepTrack(-1); return true;
        case Role::navRight: stepTrack(1); return true;
        case Role::scale: setMenuShown(!menu); return true;
        case Role::layout:
        {
            const int t = track();
            if (t < 0)
                return true;
            const auto tuning = tuningOf(t);
            const auto kinds = NoteLayout::kindsFor(*tuning);
            const auto l = layout();
            const auto at = std::find(kinds.begin(), kinds.end(), l.kind);
            const auto next = kinds[static_cast<std::size_t>(((at - kinds.begin()) + 1) % static_cast<long>(kinds.size()))];
            setLayout(l.withKind(next, *tuning));
            return true;
        }
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
    auto l = layout();
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
}

void NoteMode::leave()
{
    pressedNotes.fill(0);
    padNotes.clear();
    menu = false;
    stripLast = -1.0f;
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

    if (menu)
    {
        renderMenu(frame);
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
    const bool canDown = t >= 0 && !menu && l.canShift(-1, rows, columns);
    const bool canUp = t >= 0 && !menu && l.canShift(1, rows, columns);
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
    setButton(frame, {Role::layout, 0}, t >= 0 && NoteLayout::kindsFor(*tuningOf(t)).size() > 1 ? available : LedState::off());

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
    const auto& set = host.context().session.set();
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
        kinds.choices.push_back({NoteLayout::kindName(k), l.kind == k, [this, k, tuning] { setLayout(layout().withKind(k, *tuning)); }});
    settings.push_back(std::move(kinds));

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
