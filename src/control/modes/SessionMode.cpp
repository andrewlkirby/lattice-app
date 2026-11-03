#include "control/modes/SessionMode.h"

#include "core/model/LiveSet.h"

#include <algorithm>

namespace lattice::control
{

using ui::SessionSelection;

namespace
{
std::uint32_t colourOf(const juce::ValueTree& node, std::uint32_t fallback)
{
    // A scene's colour is 0 until one is chosen: no colour, not black.
    const auto argb = static_cast<std::uint32_t>(static_cast<juce::int64>(node.getProperty(ids::colour, 0)));
    return (argb >> 24) == 0 ? fallback : rgbOf(argb);
}

constexpr std::uint32_t defaultClipColour = 0x808080;
constexpr float trackDim = 0.35f; // an unselected track's button
constexpr float armedEmptyDim = 0.2f;
} // namespace

SessionMode::SessionMode(ModeHost& h) : Mode(h) {}

// --- The box ----------------------------------------------------------------------------------------------

std::vector<int> SessionMode::columnsOfSet() const
{
    const auto& set = host.context().session.set();
    std::vector<int> out;
    for (int t = 0; t < set.numTracks();)
    {
        out.push_back(t);
        const bool folded = set.isGroupTrack(t) && static_cast<bool>(set.track(t)[ids::folded]);
        t = folded ? set.blockEnd(t) : t + 1;
    }
    return out;
}

int SessionMode::trackColumns() const
{
    return std::max(1, caps().columns - (scenesOnGrid() ? 1 : 0));
}

int SessionMode::sceneRows() const
{
    return std::max(1, caps().rows - (trackRowShown() ? 1 : 0));
}

bool SessionMode::trackRowShown() const
{
    const bool trackButtons = caps().has(ButtonId{Role::trackSelect, 0}) || caps().has(ButtonId{Role::displayLower, 0});
    return heldModifier() != Modifier::none && !trackButtons && caps().rows > 1;
}

bool SessionMode::overviewShown() const
{
    return host.isHeld(Role::modeSession);
}

void SessionMode::setOrigin(int column, int scene)
{
    const int columns = static_cast<int>(columnsOfSet().size());
    const int scenes = host.context().session.set().numScenes();
    column0 = juce::jlimit(0, std::max(0, columns - trackColumns()), column);
    scene0 = juce::jlimit(0, std::max(0, scenes - sceneRows()), scene);
    host.requestRefresh();
}

std::optional<std::pair<int, int>> SessionMode::slotAt(int row, int column) const
{
    if (overviewShown() || row < 0 || column < 0 || column >= trackColumns() || row >= sceneRows())
        return std::nullopt;
    const auto columns = columnsOfSet();
    const int c = column0 + column;
    const int scene = scene0 + row;
    if (c >= static_cast<int>(columns.size()) || scene >= host.context().session.set().numScenes())
        return std::nullopt;
    return std::pair{columns[static_cast<std::size_t>(c)], scene};
}

int SessionMode::trackOfColumn(int gridColumn) const
{
    const auto columns = columnsOfSet();
    const int c = column0 + gridColumn;
    return gridColumn >= 0 && gridColumn < trackColumns() && c < static_cast<int>(columns.size())
               ? columns[static_cast<std::size_t>(c)]
               : -1;
}

SessionMode::Modifier SessionMode::heldModifier() const
{
    if (host.isHeld(Role::stopClip))
        return Modifier::stopClip;
    if (host.isHeld(Role::mute))
        return Modifier::mute;
    if (host.isHeld(Role::solo))
        return Modifier::solo;
    return Modifier::none;
}

std::pair<int, int> SessionMode::pageCounts() const
{
    const int columns = static_cast<int>(columnsOfSet().size());
    const int scenes = host.context().session.set().numScenes();
    return {(columns + trackColumns() - 1) / trackColumns(), (scenes + sceneRows() - 1) / sceneRows()};
}

// --- Input ------------------------------------------------------------------------------------------------

void SessionMode::enter()
{
    showSelection();
}

void SessionMode::selectionChanged()
{
    showSelection();
}

void SessionMode::showSelection()
{
    const auto sel = host.context().selection.get();
    int column = column0, scene = scene0;
    if (sel.kind != SessionSelection::Kind::scene)
    {
        const auto columns = columnsOfSet();
        const auto it = std::find(columns.begin(), columns.end(), sel.track);
        if (it != columns.end())
        {
            const int c = static_cast<int>(it - columns.begin());
            if (c < column)
                column = c;
            else if (c >= column + trackColumns())
                column = c - trackColumns() + 1;
        }
    }
    if (sel.kind != SessionSelection::Kind::track)
    {
        if (sel.scene < scene)
            scene = sel.scene;
        else if (sel.scene >= scene + sceneRows())
            scene = sel.scene - sceneRows() + 1;
    }
    if (column != column0 || scene != scene0)
        setOrigin(column, scene);
}

bool SessionMode::handle(const SurfaceInput& input)
{
    using Kind = SurfaceInput::Kind;
    const int columns = caps().columns;
    switch (input.kind)
    {
    case Kind::padDown:
    {
        const int index = input.row * columns + input.column;
        if (overviewShown())
        {
            const auto [across, down] = pageCounts();
            if (input.column < across && input.row < down)
            {
                setOrigin(input.column * trackColumns(), input.row * sceneRows());
                if (host.followsSelection())
                {
                    const int track = trackOfColumn(0);
                    if (track >= 0)
                        host.context().selection.set({SessionSelection::Kind::slot, track, scene0});
                }
            }
            return true;
        }
        if (const auto slot = slotAt(input.row, input.column))
        {
            pressSlot(index, slot->first, slot->second, input.played);
            return true;
        }
        if (scenesOnGrid() && input.column == columns - 1 && input.row < sceneRows())
        {
            if (!input.played) // the fast path launched it already
                pressScene(scene0 + input.row);
            return true;
        }
        if (trackRowShown() && input.row == caps().rows - 1)
        {
            if (const int track = trackOfColumn(input.column); track >= 0)
                pressTrack(track);
            return true;
        }
        return true;
    }
    case Kind::padUp:
    {
        const int index = input.row * columns + input.column;
        if (const auto it = pressed.find(index); it != pressed.end())
        {
            if (!input.played) // the fast path released it already
                host.context().session.releaseSlot(it->second.first, it->second.second);
            pressed.erase(it);
        }
        return true;
    }
    case Kind::buttonDown:
    {
        const auto id = input.button;
        switch (id.role)
        {
        case Role::scene:
            if (!input.played)
                pressScene(scene0 + id.index);
            return true;
        case Role::trackSelect: if (const int t = trackOfColumn(id.index); t >= 0) pressTrack(t); return true;
        case Role::displayLower:
            if (!caps().has(ButtonId{Role::trackSelect, 0}))
                if (const int t = trackOfColumn(id.index); t >= 0)
                    pressTrack(t);
            return true;
        case Role::navUp: navigate(0, -1); return true;
        case Role::navDown: navigate(0, 1); return true;
        case Role::navLeft: navigate(-1, 0); return true;
        case Role::navRight: navigate(1, 0); return true;
        default: return false;
        }
    }
    default:
        return false;
    }
}

void SessionMode::padMap(PadNoteMap& map)
{
    for (const auto role : {Role::shift, Role::deleteClip, Role::duplicate, Role::stopClip, Role::mute, Role::solo, Role::modeSession})
        map.addModifier(role);
    if (overviewShown() || trackRowShown())
        return;
    auto& session = host.context().session;
    const int numScenes = session.set().numScenes();
    const auto sceneLaunch = [](int scene) {
        EngineCommand c;
        c.type = CommandType::LaunchScene;
        c.scene = scene;
        return c;
    };
    for (int i = 0; i < caps().countOf(Role::scene); ++i)
        if (scene0 + i < numScenes)
            map.buttons.push_back({ButtonId{Role::scene, i}, sceneLaunch(scene0 + i)});
    if (scenesOnGrid())
        for (int r = 0; r < std::min(map.rows, sceneRows()); ++r)
            if (scene0 + r < numScenes)
            {
                auto& pad = map.pad(r, map.columns - 1);
                pad.kind = PadNoteMap::Pad::Kind::command;
                pad.onDown = sceneLaunch(scene0 + r);
            }
    for (int r = 0; r < map.rows; ++r)
        for (int col = 0; col < map.columns; ++col)
        {
            const auto slot = slotAt(r, col);
            if (!slot)
                continue;
            const auto direct = session.directLaunch(slot->first, slot->second);
            if (!direct)
                continue;
            auto& pad = map.pad(r, col);
            pad.kind = PadNoteMap::Pad::Kind::command;
            pad.onDown = direct->down;
            pad.hasUp = direct->up.has_value();
            if (direct->up)
                pad.onUp = *direct->up;
        }
}

void SessionMode::pressSlot(int padIndex, int track, int scene, bool played)
{
    auto& c = host.context();
    auto& set = c.session.set();
    const bool group = set.isGroupTrack(track);
    // Armed before the press selects the track (auto-arm): the first press on an empty slot of a
    // track that wasn't armed selects (and arms) it rather than starting a recording at once.
    const bool wasArmed = c.session.isArmed(track);
    if (host.followsSelection())
        c.selection.set({SessionSelection::Kind::slot, track, scene});
    if (host.shiftHeld() || played)
        return; // select only (or the fast path launched it already)
    const auto clip = set.clip(track, scene);
    if (host.isHeld(Role::deleteClip))
    {
        if (clip.isValid() && !group)
        {
            c.session.beginEdit("Delete clip", clip);
            set.deleteClip(track, scene);
        }
        return;
    }
    if (host.isHeld(Role::duplicate))
    {
        if (clip.isValid() && !group)
        {
            c.session.beginEdit("Duplicate clip", clip);
            set.duplicateClip(track, scene);
        }
        return;
    }
    if (!clip.isValid() && !group && !wasArmed)
    {
        if (set.hasStopButton(track, scene))
            c.session.stopTrack(track);
        return;
    }
    c.session.launchSlot(track, scene);
    pressed[padIndex] = {track, scene};
}

void SessionMode::pressScene(int scene)
{
    auto& c = host.context();
    auto& set = c.session.set();
    if (scene < 0 || scene >= set.numScenes())
        return;
    if (host.shiftHeld())
    {
        if (host.followsSelection())
            c.selection.set({SessionSelection::Kind::scene, 0, scene});
        return;
    }
    if (host.isHeld(Role::deleteClip))
    {
        if (set.numScenes() > 1)
        {
            c.session.beginEdit("Delete scene", set.scene(scene));
            set.deleteScene(scene);
        }
        return;
    }
    if (host.isHeld(Role::duplicate))
    {
        c.session.beginEdit("Duplicate scene", set.scene(scene));
        set.duplicateScene(scene);
        return;
    }
    // Launching doesn't select the scene: that would take the selection (and auto-arm) off the
    // track being played.
    c.session.launchScene(scene);
}

void SessionMode::pressTrack(int track)
{
    auto& c = host.context();
    auto& set = c.session.set();
    const auto node = set.track(track);
    switch (heldModifier())
    {
    case Modifier::stopClip: c.session.stopTrack(track); return;
    case Modifier::mute:
        c.session.beginEdit("Mute", node);
        set.setTrackMute(track, !static_cast<bool>(node[ids::mute]));
        return;
    case Modifier::solo:
        c.session.beginEdit("Solo", node);
        set.setTrackSolo(track, !static_cast<bool>(node[ids::solo]), host.shiftHeld());
        return;
    case Modifier::none:
        if (host.followsSelection())
            c.selection.set({SessionSelection::Kind::track, track, 0});
        return;
    }
}

void SessionMode::navigate(int dx, int dy)
{
    const bool page = host.shiftHeld();
    const int stepX = page ? trackColumns() : 1;
    const int stepY = page ? sceneRows() : 1;
    if (page)
        setOrigin(column0 + dx * stepX, scene0 + dy * stepY);
    if (!host.followsSelection())
    {
        if (!page)
            setOrigin(column0 + dx, scene0 + dy);
        return;
    }
    auto& c = host.context();
    const auto columns = columnsOfSet();
    if (columns.empty())
        return;
    auto sel = c.selection.get();
    const auto it = std::find(columns.begin(), columns.end(), sel.track);
    const int column = it == columns.end() ? column0 : static_cast<int>(it - columns.begin());
    const int scenes = c.session.set().numScenes();
    SessionSelection next;
    next.kind = sel.kind;
    if (dx != 0 && next.kind == SessionSelection::Kind::scene)
        next.kind = SessionSelection::Kind::slot;
    if (dy != 0 && next.kind == SessionSelection::Kind::track)
        next.kind = SessionSelection::Kind::slot;
    next.track = columns[static_cast<std::size_t>(juce::jlimit(0, static_cast<int>(columns.size()) - 1, column + dx * stepX))];
    next.scene = juce::jlimit(0, std::max(0, scenes - 1), sel.scene + dy * stepY);
    c.selection.set(next);
}

// --- LEDs -------------------------------------------------------------------------------------------------

LedState SessionMode::slotLed(int track, int scene) const
{
    auto& c = host.context();
    const auto& colours = host.colours();
    const auto& set = c.session.set();
    const auto state = c.feed.slot(track, scene);
    if (set.isGroupTrack(track))
    {
        switch (state)
        {
        case SlotState::Playing:
        case SlotState::Recording: return LedState::pulse(colours.white);
        case SlotState::QueuedLaunch:
        case SlotState::QueuedStop:
        case SlotState::QueuedRecord: return LedState::blink(colours.white);
        case SlotState::Stopped: return LedState::solid(dimmed(colours.white, 0.5f));
        case SlotState::Empty: return LedState::off();
        }
        return LedState::off();
    }
    const auto clip = set.clip(track, scene);
    switch (state)
    {
    case SlotState::Recording: return LedState::pulse(colours.record);
    case SlotState::QueuedRecord: return LedState::blink(colours.record);
    default: break;
    }
    if (clip.isValid())
    {
        const auto colour = colourOf(clip, defaultClipColour);
        switch (state)
        {
        case SlotState::Playing: return LedState::pulse(colour);
        case SlotState::QueuedLaunch:
        case SlotState::QueuedStop: return LedState::blink(colour);
        default: return LedState::solid(colour);
        }
    }
    if (c.session.isArmed(track))
        return LedState::solid(dimmed(colours.arm, armedEmptyDim));
    return LedState::off();
}

LedState SessionMode::sceneLed(int scene) const
{
    auto& c = host.context();
    const auto& colours = host.colours();
    const auto& set = c.session.set();
    if (scene < 0 || scene >= set.numScenes())
        return LedState::off();
    bool clips = false;
    for (int t = 0; t < set.numTracks(); ++t)
    {
        if (set.isGroupTrack(t))
            continue;
        if (c.feed.slot(t, scene) == SlotState::QueuedLaunch)
            return LedState::blink(colours.play);
        clips = clips || set.clip(t, scene).isValid();
    }
    const auto colour = colourOf(set.scene(scene), colours.white);
    return LedState::solid(dimmed(colour, clips ? 0.6f : 0.12f));
}

LedState SessionMode::trackLed(int track) const
{
    auto& c = host.context();
    const auto& colours = host.colours();
    const auto& set = c.session.set();
    if (track < 0 || track >= set.numTracks())
        return LedState::off();
    const auto node = set.track(track);
    switch (heldModifier())
    {
    case Modifier::stopClip:
    {
        bool playing = false, stopping = false;
        const auto leaves = set.isGroupTrack(track) ? set.groupLeaves(track) : std::vector<int>{track};
        for (const int leaf : leaves)
        {
            const auto& play = c.feed.trackPlay(leaf);
            playing = playing || play.playingClipId != 0;
            stopping = stopping || play.queued == TrackPlayState::Queued::Stop;
        }
        return stopping ? LedState::blink(colours.record)
                        : playing ? LedState::solid(colours.record) : LedState::solid(dimmed(colours.record, colours.dim * 0.5f));
    }
    case Modifier::mute:
        return static_cast<bool>(node[ids::mute]) ? LedState::solid(dimmed(colours.mute, 0.1f)) : LedState::solid(colours.mute);
    case Modifier::solo:
        return static_cast<bool>(node[ids::solo]) ? LedState::solid(colours.solo) : LedState::solid(dimmed(colours.solo, 0.1f));
    case Modifier::none: break;
    }
    const auto sel = c.selection.get();
    if (sel.kind != SessionSelection::Kind::scene && sel.containsTrack(track))
        return LedState::solid(colours.white);
    return LedState::solid(dimmed(colourOf(node, colours.white), trackDim));
}

void SessionMode::renderOverview(LedFrame& frame) const
{
    auto& c = host.context();
    const auto& colours = host.colours();
    const auto& set = c.session.set();
    const auto columns = columnsOfSet();
    const auto [across, down] = pageCounts();
    const int width = trackColumns(), height = sceneRows();
    const int currentX = column0 / width, currentY = scene0 / height;
    for (int r = 0; r < caps().rows; ++r)
        for (int col = 0; col < caps().columns; ++col)
        {
            auto& led = frame.pad(r, col, caps().columns);
            if (col >= across || r >= down)
                continue;
            if (col == currentX && r == currentY)
            {
                led = LedState::solid(colours.white);
                continue;
            }
            bool clips = false, playing = false;
            for (int i = col * width; i < std::min(static_cast<int>(columns.size()), (col + 1) * width); ++i)
                for (int s = r * height; s < std::min(set.numScenes(), (r + 1) * height); ++s)
                {
                    const int t = columns[static_cast<std::size_t>(i)];
                    playing = playing || c.feed.slot(t, s) == SlotState::Playing;
                    clips = clips || set.clip(t, s).isValid();
                }
            led = playing ? LedState::solid(colours.play) : LedState::solid(dimmed(colours.white, clips ? 0.35f : 0.08f));
        }
}

void SessionMode::render(LedFrame& frame)
{
    const auto& colours = host.colours();
    const auto& set = host.context().session.set();
    const int rows = caps().rows, columns = caps().columns;
    if (overviewShown())
        renderOverview(frame);
    else
        for (int r = 0; r < rows; ++r)
            for (int col = 0; col < columns; ++col)
            {
                auto& led = frame.pad(r, col, columns);
                if (const auto slot = slotAt(r, col))
                    led = slotLed(slot->first, slot->second);
                else if (scenesOnGrid() && col == columns - 1 && r < sceneRows())
                    led = sceneLed(scene0 + r);
                else if (trackRowShown() && r == rows - 1)
                    led = trackLed(trackOfColumn(col));
            }

    for (int i = 0, n = caps().countOf(Role::scene); i < n; ++i)
        setButton(frame, {Role::scene, i}, sceneLed(scene0 + i));
    const auto trackRole = caps().has(ButtonId{Role::trackSelect, 0}) ? Role::trackSelect : Role::displayLower;
    for (int i = 0, n = caps().countOf(trackRole); i < n; ++i)
        setButton(frame, {trackRole, i}, trackLed(trackOfColumn(i)));

    const auto available = LedState::solid(dimmed(colours.white, colours.dim));
    const auto lit = LedState::solid(colours.white);
    for (const auto role : {Role::deleteClip, Role::duplicate, Role::stopClip, Role::mute, Role::solo})
        setButton(frame, {role, 0}, host.isHeld(role) ? lit : available);

    const auto sel = host.context().selection.get();
    const auto columnsList = columnsOfSet();
    const int count = static_cast<int>(columnsList.size());
    const auto it = std::find(columnsList.begin(), columnsList.end(), sel.track);
    const int selColumn = it == columnsList.end() ? -1 : static_cast<int>(it - columnsList.begin());
    const bool follow = host.followsSelection();
    const auto can = [&](bool possible) { return possible ? available : LedState::off(); };
    setButton(frame, {Role::navUp, 0}, can(scene0 > 0 || (follow && sel.scene > 0)));
    setButton(frame, {Role::navDown, 0}, can(scene0 + sceneRows() < set.numScenes() || (follow && sel.scene < set.numScenes() - 1)));
    setButton(frame, {Role::navLeft, 0}, can(column0 > 0 || (follow && selColumn > 0)));
    setButton(frame, {Role::navRight, 0}, can(column0 + trackColumns() < count || (follow && selColumn >= 0 && selColumn < count - 1)));
}

} // namespace lattice::control
