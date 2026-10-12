#include "app/debug/SessionGrid.h"

#include "app/debug/ClipPropertiesPanel.h"

namespace lattice::debugui
{
namespace
{
bool isBlinking(SlotState s)
{
    return s == SlotState::QueuedLaunch || s == SlotState::QueuedStop || s == SlotState::QueuedRecord;
}

void drawPlayIcon(juce::Graphics& g, juce::Rectangle<float> r, juce::Colour colour, bool filled)
{
    juce::Path p;
    p.addTriangle(r.getX(), r.getY(), r.getX(), r.getBottom(), r.getRight(), r.getCentreY());
    g.setColour(colour);
    if (filled)
        g.fillPath(p);
    else
        g.strokePath(p, juce::PathStrokeType(1.2f));
}
} // namespace

SessionGrid::SessionGrid(SessionController& s, Selection& sel) : session(s), selection(sel)
{
    setOpaque(true);
    setWantsKeyboardFocus(false);
}

void SessionGrid::setColumnWidth(int width)
{
    trackWidth = width;
    repaint();
}

int SessionGrid::preferredWidth() const
{
    return session.set().numTracks() * trackWidth + sceneColumnWidth;
}

int SessionGrid::preferredHeight() const
{
    return headerHeight + session.set().numScenes() * rowHeight + stopRowHeight;
}

juce::Rectangle<int> SessionGrid::slotBounds(int track, int scene) const
{
    return {track * trackWidth, headerHeight + scene * rowHeight, trackWidth, rowHeight};
}

SessionGrid::Hit SessionGrid::findHit(juce::Point<int> p) const
{
    const auto& set = session.set();
    const int tracks = set.numTracks();
    const int scenes = set.numScenes();
    const int column = p.x / trackWidth;
    const bool inSceneColumn = p.x >= tracks * trackWidth;
    const int stopRowTop = headerHeight + scenes * rowHeight;

    if (p.y < headerHeight)
        return inSceneColumn ? Hit{Area::SceneHeader, -1, -1} : Hit{Area::TrackHeader, column, -1};
    if (p.y >= stopRowTop)
        return inSceneColumn ? Hit{Area::StopAll, -1, -1} : Hit{Area::TrackStop, column, -1};
    const int row = (p.y - headerHeight) / rowHeight;
    if (row < 0 || row >= scenes)
        return {};
    if (inSceneColumn)
        return {Area::Scene, -1, row};
    if (column >= 0 && column < tracks)
        return {Area::Slot, column, row};
    return {};
}

void SessionGrid::poll()
{
    const auto& set = session.set();
    const int tracks = set.numTracks();
    const int scenes = set.numScenes();
    const auto count = static_cast<std::size_t>(tracks * scenes);
    if (lastStates.size() != count || lastPositions.size() != static_cast<std::size_t>(tracks))
    {
        lastStates.assign(count, SlotState::Empty);
        lastPositions.assign(static_cast<std::size_t>(tracks), -1.0);
        repaint();
    }

    const bool blink = (juce::Time::getMillisecondCounter() / 250) % 2 == 0;
    const bool blinkChanged = blink != lastBlink;
    lastBlink = blink;

    for (int t = 0; t < tracks; ++t)
    {
        const auto play = session.trackState(t);
        bool trackChanged = false;
        for (int s = 0; s < scenes; ++s)
        {
            const auto state = session.slotState(t, s);
            auto& last = lastStates[static_cast<std::size_t>(t * scenes + s)];
            if (state != last || (blinkChanged && isBlinking(state)))
                repaint(slotBounds(t, s));
            trackChanged = trackChanged || state != last;
            last = state;
        }

        // The playing slot's progress bar, about 30 times a second while it moves.
        const double position = play.playingClipId != 0 ? play.position : -1.0;
        auto& lastPosition = lastPositions[static_cast<std::size_t>(t)];
        if (position != lastPosition)
        {
            const auto [pt, ps] = set.findClip(play.playingClipId);
            if (pt == t)
                repaint(slotBounds(t, ps));
            lastPosition = position;
        }
        if (trackChanged)
            repaint(juce::Rectangle<int>(t * trackWidth, headerHeight + scenes * rowHeight, trackWidth,
                                         stopRowHeight));
    }
}

void SessionGrid::paint(juce::Graphics& g)
{
    const auto& set = session.set();
    const int tracks = set.numTracks();
    const int scenes = set.numScenes();
    const bool blink = (juce::Time::getMillisecondCounter() / 250) % 2 == 0;
    const auto clip = g.getClipBounds();

    g.fillAll(colours::background);
    g.setFont(juce::FontOptions(13.0f));

    // Track headers
    for (int t = 0; t < tracks; ++t)
    {
        const auto r = juce::Rectangle<int>(t * trackWidth, 0, trackWidth, headerHeight).reduced(1);
        if (!r.intersects(clip))
            continue;
        const auto track = set.track(t);
        const bool selected = !selection.sceneColumn && selection.track == t;
        g.setColour(selected ? colours::panel.brighter(0.25f) : colours::panel);
        g.fillRect(r);
        g.setColour(juce::Colour(static_cast<juce::uint32>(static_cast<juce::int64>(track[ids::colour]))));
        g.fillRect(r.withHeight(3));
        g.setColour(colours::text);
        juce::String name = track[ids::name].toString();
        if (static_cast<bool>(track[ids::arm]))
            name = juce::String::fromUTF8("\xe2\x97\x8f ") + name; // ●
        g.drawFittedText(name, r.reduced(4, 0), juce::Justification::centredLeft, 1);
    }
    {
        const auto r =
            juce::Rectangle<int>(tracks * trackWidth, 0, sceneColumnWidth, headerHeight).reduced(1);
        g.setColour(colours::panel);
        g.fillRect(r);
        g.setColour(colours::dimText);
        g.drawFittedText("Scenes", r.reduced(4, 0), juce::Justification::centredLeft, 1);
    }

    // Slots
    for (int t = 0; t < tracks; ++t)
        for (int s = 0; s < scenes; ++s)
            if (slotBounds(t, s).intersects(clip))
                paintSlot(g, t, s, session.slotState(t, s), blink);

    // Scene launch column
    for (int s = 0; s < scenes; ++s)
    {
        const auto r = juce::Rectangle<int>(tracks * trackWidth, headerHeight + s * rowHeight,
                                            sceneColumnWidth, rowHeight)
                           .reduced(2);
        if (!r.intersects(clip))
            continue;
        const auto scene = set.scene(s);
        g.setColour(colours::panel.brighter(0.08f));
        g.fillRoundedRectangle(r.toFloat(), 3.0f);
        drawPlayIcon(g, r.toFloat().removeFromLeft(18.0f).reduced(5.0f, 8.0f), colours::text, false);

        juce::String text = scene[ids::name].toString();
        if (text.isEmpty())
            text = "Scene " + juce::String(s + 1);
        if (const double tempo = scene[ids::sceneTempo]; tempo > 0.0)
            text << "  " << juce::String(tempo, 1) << " BPM";
        if (const int num = scene[ids::sceneTimeSigNum]; num > 0)
            text << "  " << num << "/" << static_cast<int>(scene[ids::sceneTimeSigDen]);
        g.setColour(colours::text);
        g.drawFittedText(text, r.withTrimmedLeft(20).reduced(2, 0), juce::Justification::centredLeft, 1);

        if (selection.sceneColumn && selection.scene == s)
        {
            g.setColour(colours::selection);
            g.drawRoundedRectangle(r.toFloat(), 3.0f, 1.5f);
        }
    }

    // Track stop buttons and Stop All
    const int stopTop = headerHeight + scenes * rowHeight;
    for (int t = 0; t < tracks; ++t)
    {
        const auto r = juce::Rectangle<int>(t * trackWidth, stopTop, trackWidth, stopRowHeight).reduced(2);
        if (!r.intersects(clip))
            continue;
        const bool active = session.trackState(t).playingClipId != 0;
        g.setColour(colours::panel);
        g.fillRoundedRectangle(r.toFloat(), 3.0f);
        g.setColour(active ? colours::text : colours::dimText);
        g.fillRect(r.withSizeKeepingCentre(10, 10));
    }
    {
        const auto r =
            juce::Rectangle<int>(tracks * trackWidth, stopTop, sceneColumnWidth, stopRowHeight).reduced(2);
        const bool queued = session.transport().stopAllQueued;
        g.setColour(queued && blink ? colours::panel.brighter(0.3f) : colours::panel);
        g.fillRoundedRectangle(r.toFloat(), 3.0f);
        g.setColour(colours::text);
        g.drawFittedText("Stop All Clips", r, juce::Justification::centred, 1);
    }
}

void SessionGrid::paintSlot(juce::Graphics& g, int track, int scene, SlotState state, bool blinkOn)
{
    const auto& set = session.set();
    const auto bounds = slotBounds(track, scene).reduced(2);
    const auto r = bounds.toFloat();
    const auto clip = set.clip(track, scene);
    const bool armed = set.track(track)[ids::arm];
    auto icon = r.withWidth(18.0f).reduced(5.0f, 8.0f);

    if (!clip.isValid())
    {
        g.setColour(colours::emptySlot);
        g.fillRoundedRectangle(r, 3.0f);
        if (armed)
        {
            g.setColour(colours::record.withAlpha(0.6f));
            g.drawEllipse(icon.withSizeKeepingCentre(9.0f, 9.0f), 1.2f);
        }
        else if (set.hasStopButton(track, scene))
        {
            g.setColour(colours::dimText.withAlpha(0.5f));
            g.fillRect(icon.withSizeKeepingCentre(7.0f, 7.0f));
        }
    }
    else
    {
        const auto colour =
            juce::Colour(static_cast<juce::uint32>(static_cast<juce::int64>(clip[ids::colour])));
        const bool playing =
            state == SlotState::Playing || state == SlotState::Recording || state == SlotState::QueuedStop;
        g.setColour(playing ? colour : colour.withMultipliedBrightness(0.55f).withMultipliedSaturation(0.8f));
        g.fillRoundedRectangle(r, 3.0f);

        const auto dark = juce::Colour(0xff111111);
        switch (state)
        {
        case SlotState::Playing:
            drawPlayIcon(g, icon, colours::play.darker(0.3f), true);
            break;
        case SlotState::QueuedLaunch:
            drawPlayIcon(g, icon, blinkOn ? colours::play.darker(0.2f) : dark, blinkOn);
            break;
        case SlotState::QueuedStop:
            drawPlayIcon(g, icon, blinkOn ? colours::play.darker(0.3f) : dark, true);
            break;
        case SlotState::QueuedRecord:
            g.setColour(blinkOn ? colours::record : dark);
            g.fillEllipse(icon.withSizeKeepingCentre(9.0f, 9.0f));
            break;
        case SlotState::Recording:
            g.setColour(colours::record);
            g.fillEllipse(icon.withSizeKeepingCentre(10.0f, 10.0f));
            break;
        case SlotState::Stopped:
        case SlotState::Empty:
            drawPlayIcon(g, icon, dark, false);
            break;
        }

        juce::String name = clip[ids::name].toString();
        if (name.isEmpty())
            name = juce::String(LiveSet::notesOf(clip).size()) + " notes";
        g.setColour(dark);
        g.drawFittedText(name, bounds.withTrimmedLeft(20).reduced(2, 0), juce::Justification::centredLeft, 1);

        if (playing)
        {
            const auto play = session.trackState(track);
            const double length = play.loopEnd - play.loopStart;
            if (length > 0.0 && play.playingClipId == LiveSet::idOf(clip))
            {
                const double fraction = juce::jlimit(0.0, 1.0, (play.position - play.loopStart) / length);
                auto bar = r.withTrimmedTop(r.getHeight() - 3.0f).reduced(3.0f, 0.0f);
                g.setColour(dark.withAlpha(0.5f));
                g.fillRect(bar.withWidth(static_cast<float>(bar.getWidth() * std::max(0.0, fraction))));
            }
        }
    }

    if (!selection.sceneColumn && selection.track == track && selection.scene == scene)
    {
        g.setColour(colours::selection);
        g.drawRoundedRectangle(bounds.toFloat(), 3.0f, 1.5f);
    }
}

void SessionGrid::select(int track, int scene, bool sceneColumn)
{
    if (track >= 0)
        selection.track = track;
    if (scene >= 0)
        selection.scene = scene;
    selection.sceneColumn = sceneColumn;
    repaint();
    if (onSelectionChanged)
        onSelectionChanged();
}

void SessionGrid::mouseDown(const juce::MouseEvent& e)
{
    const auto hit = findHit(e.getPosition());
    if (e.mods.isPopupMenu())
    {
        if (hit.area == Area::Slot)
        {
            select(hit.track, hit.scene, false);
            showSlotMenu(hit.track, hit.scene);
        }
        else if (hit.area == Area::Scene)
        {
            select(-1, hit.scene, true);
            showSceneMenu(hit.scene);
        }
        else if (hit.area == Area::TrackHeader && hit.track < session.set().numTracks())
        {
            select(hit.track, -1, false);
            showTrackMenu(hit.track);
        }
        return;
    }

    switch (hit.area)
    {
    case Area::Slot:
        select(hit.track, hit.scene, false);
        session.launchSlot(hit.track, hit.scene);
        pressed = hit;
        break;
    case Area::Scene:
        session.launchScene(hit.scene);
        select(-1,
               session.set().selectNextSceneOnLaunch()
                   ? std::min(hit.scene + 1, session.set().numScenes() - 1)
                   : hit.scene,
               true);
        break;
    case Area::TrackHeader:
        if (hit.track < session.set().numTracks())
            select(hit.track, -1, false);
        break;
    case Area::TrackStop:
        if (hit.track < session.set().numTracks())
            session.stopTrack(hit.track);
        break;
    case Area::StopAll:
        session.stopAllClips();
        break;
    case Area::SceneHeader:
    case Area::None:
        break;
    }
}

void SessionGrid::mouseUp(const juce::MouseEvent&)
{
    if (pressed.area == Area::Slot)
        session.releaseSlot(pressed.track, pressed.scene);
    pressed = {};
}

void SessionGrid::mouseDoubleClick(const juce::MouseEvent& e)
{
    const auto hit = findHit(e.getPosition());
    auto& set = session.set();
    if (hit.area == Area::TrackHeader && hit.track < set.numTracks())
    {
        askForText("Rename track", {}, set.track(hit.track)[ids::name].toString(),
                   [this, t = hit.track](const juce::String& text) {
                       session.undoManager().beginNewTransaction("Rename track");
                       session.set().setTrackName(t, text);
                   });
    }
    else if (hit.area == Area::Slot && !set.clip(hit.track, hit.scene).isValid() &&
             !static_cast<bool>(set.track(hit.track)[ids::arm]))
    {
        session.undoManager().beginNewTransaction("Create clip");
        set.createClip(hit.track, hit.scene, set.beatsPerBar());
    }
}

void SessionGrid::showSlotMenu(int track, int scene)
{
    auto& set = session.set();
    const bool hasClip = set.clip(track, scene).isValid();
    juce::PopupMenu menu;
    menu.addItem(1, "Import MIDI file...");
    menu.addItem(2, "Create clip (1 bar)", !hasClip);
    menu.addItem(3, "Create clip (4 bars)", !hasClip);
    menu.addSeparator();
    menu.addItem(4, "Clip properties...", hasClip);
    menu.addItem(5, "Duplicate clip", hasClip);
    menu.addItem(6, "Delete clip", hasClip);
    menu.addSeparator();
    menu.addItem(7, "Stop button", !hasClip, set.hasStopButton(track, scene));

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
                       [this, track, scene](int result) {
                           auto& s = session.set();
                           auto& undo = session.undoManager();
                           if (track >= s.numTracks() || scene >= s.numScenes())
                               return;
                           switch (result)
                           {
                           case 1:
                               if (onImportMidi)
                                   onImportMidi(track, scene);
                               break;
                           case 2:
                           case 3:
                               undo.beginNewTransaction("Create clip");
                               s.createClip(track, scene, s.beatsPerBar() * (result == 2 ? 1 : 4));
                               break;
                           case 4:
                               showClipProperties(track, scene);
                               break;
                           case 5:
                               undo.beginNewTransaction("Duplicate clip");
                               s.duplicateClip(track, scene);
                               break;
                           case 6:
                               undo.beginNewTransaction("Delete clip");
                               s.deleteClip(track, scene);
                               break;
                           case 7:
                               undo.beginNewTransaction("Stop button");
                               s.setHasStopButton(track, scene, !s.hasStopButton(track, scene));
                               break;
                           default:
                               break;
                           }
                       });
}

void SessionGrid::showClipProperties(int track, int scene)
{
    const auto clip = session.set().clip(track, scene);
    if (!clip.isValid())
        return;
    auto panel = std::make_unique<ClipPropertiesPanel>(session, clip);
    juce::CallOutBox::launchAsynchronously(std::move(panel), localAreaToGlobal(slotBounds(track, scene)),
                                           nullptr);
}

void SessionGrid::showSceneMenu(int scene)
{
    juce::PopupMenu menu;
    menu.addItem(1, "Launch");
    menu.addItem(2, "Rename...");
    menu.addItem(3, "Tempo...");
    menu.addItem(4, "Time signature...");
    menu.addSeparator();
    menu.addItem(5, "Insert scene");
    menu.addItem(6, "Duplicate scene");
    menu.addItem(7, "Delete scene", session.set().numScenes() > 1);

    menu.showMenuAsync(
        juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(), [this, scene](int result) {
            auto& s = session.set();
            auto& undo = session.undoManager();
            if (scene >= s.numScenes())
                return;
            switch (result)
            {
            case 1:
                session.launchScene(scene);
                break;
            case 2:
                askForText("Rename scene", {}, s.scene(scene)[ids::name].toString(),
                           [this, scene](const juce::String& text) {
                               session.undoManager().beginNewTransaction("Rename scene");
                               session.set().setSceneName(scene, text);
                           });
                break;
            case 3:
                askForText("Scene tempo", "BPM (empty for none)",
                           juce::String(static_cast<double>(s.scene(scene)[ids::sceneTempo])),
                           [this, scene](const juce::String& text) {
                               session.undoManager().beginNewTransaction("Scene tempo");
                               session.set().setSceneTempo(scene, text.trim().getDoubleValue());
                           });
                break;
            case 4:
                askForText("Scene time signature", "e.g. 3/4 (empty for none)", {},
                           [this, scene](const juce::String& text) {
                               session.undoManager().beginNewTransaction("Scene time signature");
                               const int num =
                                   text.upToFirstOccurrenceOf("/", false, false).trim().getIntValue();
                               const int den =
                                   text.fromFirstOccurrenceOf("/", false, false).trim().getIntValue();
                               session.set().setSceneTimeSignature(scene, num, den);
                           });
                break;
            case 5:
                undo.beginNewTransaction("Insert scene");
                s.addScene(scene);
                break;
            case 6:
                undo.beginNewTransaction("Duplicate scene");
                s.duplicateScene(scene);
                break;
            case 7:
                undo.beginNewTransaction("Delete scene");
                s.deleteScene(scene);
                break;
            default:
                break;
            }
        });
}

void SessionGrid::showTrackMenu(int track)
{
    juce::PopupMenu menu;
    menu.addItem(1, "Rename...");
    menu.addItem(2, "Insert MIDI track");
    menu.addItem(3, "Duplicate track");
    menu.addItem(4, "Delete track", session.set().numTracks() > 1);
    menu.addSeparator();
    menu.addItem(5, "Move left", track > 0);
    menu.addItem(6, "Move right", track + 1 < session.set().numTracks());

    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition(),
                       [this, track](int result) {
                           auto& s = session.set();
                           auto& undo = session.undoManager();
                           if (track >= s.numTracks())
                               return;
                           switch (result)
                           {
                           case 1:
                               askForText("Rename track", {}, s.track(track)[ids::name].toString(),
                                          [this, track](const juce::String& text) {
                                              session.undoManager().beginNewTransaction("Rename track");
                                              session.set().setTrackName(track, text);
                                          });
                               break;
                           case 2:
                               undo.beginNewTransaction("Insert track");
                               s.addTrack(track + 1);
                               break;
                           case 3:
                               undo.beginNewTransaction("Duplicate track");
                               s.duplicateTrack(track);
                               break;
                           case 4:
                               undo.beginNewTransaction("Delete track");
                               s.deleteTrack(track);
                               break;
                           case 5:
                               undo.beginNewTransaction("Move track");
                               s.moveTrack(track, track - 1);
                               break;
                           case 6:
                               undo.beginNewTransaction("Move track");
                               s.moveTrack(track, track + 1);
                               break;
                           default:
                               break;
                           }
                       });
}

} // namespace lattice::debugui
