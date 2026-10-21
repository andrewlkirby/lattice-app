#include "ui/session/SessionView.h"

#include "ui/theme/LatticeLookAndFeel.h"
#include "ui/widgets/Toggle.h"

#include <cmath>

namespace lattice::ui
{
namespace
{
using Area = SessionView::Hit::Area;
using Kind = SessionSelection::Kind;

/** Is this tree inside a device (its parameters and state)? Those changes don't touch the grid. */
bool insideDevice(juce::ValueTree tree)
{
    for (; tree.isValid(); tree = tree.getParent())
        if (tree.hasType(ids::device) || tree.hasType(ids::devices))
            return true;
    return false;
}

juce::Colour colourOf(const juce::ValueTree& node)
{
    return juce::Colour(static_cast<juce::uint32>(static_cast<juce::int64>(node.getProperty(ids::colour, static_cast<juce::int64>(0xff808080)))));
}

void triangle(juce::Graphics& g, juce::Rectangle<float> r)
{
    juce::Path p;
    p.addTriangle(r.getX(), r.getY(), r.getX(), r.getBottom(), r.getRight(), r.getCentreY());
    g.fillPath(p);
}
} // namespace

void askForText(juce::Component& owner, const juce::String& title, const juce::String& initial,
                std::function<void(const juce::String&)> onOk)
{
    auto* window = new juce::AlertWindow(title, {}, juce::MessageBoxIconType::NoIcon);
    window->setLookAndFeel(&owner.getLookAndFeel());
    window->addTextEditor("text", initial);
    window->addButton("OK", 1, juce::KeyPress(juce::KeyPress::returnKey));
    window->addButton("Cancel", 0, juce::KeyPress(juce::KeyPress::escapeKey));
    window->enterModalState(true,
                            juce::ModalCallbackFunction::create(
                                [window, safe = juce::Component::SafePointer<juce::Component>(&owner), onOk = std::move(onOk)](int result) {
                                    if (result == 1 && safe != nullptr && onOk)
                                        onOk(window->getTextEditorContents("text"));
                                }),
                            true);
}

SessionView::SessionView(SessionController& s, EngineStateFeed& f, CommandRegistry& c, SessionSelectionModel& sel)
    : session(s), feed(f), commands(c), selection(sel)
{
    setTitle("Session view");
    setOpaque(true);
    for (auto* bar : {&vertical, &horizontal})
    {
        bar->setAutoHide(false);
        bar->addListener(this);
        addAndMakeVisible(*bar);
    }
    stripArea.setInterceptsMouseClicks(false, true);
    addAndMakeVisible(stripArea);
    session.set().state().addListener(this);
    selection.addListener(this);
    feed.addListener(this, EngineStateFeed::slotsAspect | EngineStateFeed::transportAspect | EngineStateFeed::metersAspect);
    rebuild();
}

SessionView::~SessionView()
{
    strips.clear();
    master.reset();
    feed.removeListener(this);
    selection.removeListener(this);
    session.set().state().removeListener(this);
}

// --- Layout ---------------------------------------------------------------------------------------

void SessionView::setZoom(float w, float h)
{
    widthZoom = juce::jlimit(0.5f, 3.0f, w);
    heightZoom = juce::jlimit(0.5f, 3.0f, h);
    rebuild();
}

void SessionView::rebuild()
{
    const auto& t = themeOf(*this);
    grid.build(session.set(), SessionLayout::metricsFor(t.controlHeight, widthZoom, heightZoom));
    rebuildStrips();
    structureChanged = false;
    resized();
    repaint();
}

void SessionView::rebuildStrips()
{
    strips.clear();
    master.reset();
    if (!showMixer)
        return;
    for (const auto& col : grid.columns())
    {
        auto strip = std::make_unique<MixerStrip>(session,
                                                  col.kind == SessionLayout::Column::Kind::track ? MixerStrip::Kind::track
                                                                                                 : MixerStrip::Kind::returnTrack,
                                                  col.index);
        strip->setShowsSends(showSends);
        stripArea.addAndMakeVisible(*strip);
        strips.push_back(std::move(strip));
    }
    master = std::make_unique<MixerStrip>(session, MixerStrip::Kind::master, 0);
    addAndMakeVisible(*master);
}

void SessionView::setMixerVisible(bool visible)
{
    if (visible == showMixer)
        return;
    showMixer = visible;
    rebuild();
}

void SessionView::setSendsVisible(bool visible)
{
    if (visible == showSends)
        return;
    showSends = visible;
    for (auto& s : strips)
        s->setShowsSends(visible);
    resized();
}

MixerStrip* SessionView::stripForColumn(int column) const
{
    return column >= 0 && column < static_cast<int>(strips.size()) ? strips[static_cast<std::size_t>(column)].get() : nullptr;
}

void SessionView::placeStrips()
{
    for (std::size_t c = 0; c < strips.size() && c < grid.columns().size(); ++c)
    {
        const auto& col = grid.columns()[c];
        strips[c]->setBounds(col.x - scroll.x, 0, col.width, stripArea.getHeight());
    }
}

void SessionView::resized()
{
    const auto& m = grid.metrics();
    auto area = getLocalBounds();
    const int bar = juce::roundToInt(themeOf(*this).spacing * 3.0f);
    horizontal.setBounds(area.removeFromBottom(bar));
    // The mixer: as tall as its strips want, but leaving the grid a few rows.
    const int mixerHeight = showMixer ? juce::jmin(MixerStrip::heightFor(*this, session.set().numReturns(), showSends),
                                                   juce::jmax(0, area.getHeight() - juce::roundToInt(m.headerHeight + m.stopRowHeight) -
                                                                     3 * grid.rowHeight()))
                                      : 0;
    auto mixerRow = area.removeFromBottom(mixerHeight);
    auto sceneSide = area.removeFromRight(juce::roundToInt(m.sceneWidth));
    scenes = sceneSide;
    vertical.setBounds(area.removeFromRight(bar));
    headers = area.removeFromTop(juce::roundToInt(m.headerHeight));
    stops = area.removeFromBottom(juce::roundToInt(m.stopRowHeight));
    slots = area;
    mixer = mixerRow;
    stripArea.setBounds(mixerRow.withWidth(slots.getRight() - mixerRow.getX()).withX(slots.getX()));
    if (master != nullptr)
        master->setBounds(mixerRow.withX(sceneSide.getX()).withWidth(sceneSide.getWidth()));
    setScrollPosition(scroll); // clamps to the new sizes
    placeStrips();
}

void SessionView::setScrollPosition(juce::Point<int> p)
{
    const int maxX = juce::jmax(0, grid.contentWidth() - slots.getWidth());
    const int maxY = juce::jmax(0, grid.rowsHeight() - slots.getHeight());
    p = {juce::jlimit(0, maxX, p.x), juce::jlimit(0, maxY, p.y)};
    if (p != scroll)
    {
        scroll = p;
        placeStrips();
        repaint();
    }
    updateScrollBars();
}

void SessionView::updateScrollBars()
{
    horizontal.setRangeLimits(0.0, juce::jmax(1, grid.contentWidth()), juce::dontSendNotification);
    horizontal.setCurrentRange(scroll.x, slots.getWidth(), juce::dontSendNotification);
    vertical.setRangeLimits(0.0, juce::jmax(1, grid.rowsHeight()), juce::dontSendNotification);
    vertical.setCurrentRange(scroll.y, slots.getHeight(), juce::dontSendNotification);
}

void SessionView::scrollBarMoved(juce::ScrollBar* bar, double start)
{
    if (bar == &horizontal)
        setScrollPosition({juce::roundToInt(start), scroll.y});
    else
        setScrollPosition({scroll.x, juce::roundToInt(start)});
}

void SessionView::revealSelection()
{
    const auto s = selection.get();
    auto p = scroll;
    if (s.kind != Kind::scene)
        if (const int c = grid.columnOfTrack(s.track); c >= 0)
        {
            const auto& col = grid.columns()[static_cast<std::size_t>(c)];
            if (col.x < p.x)
                p.x = col.x;
            else if (col.x + col.width > p.x + slots.getWidth())
                p.x = col.x + col.width - slots.getWidth();
        }
    if (s.kind != Kind::track)
    {
        const int y = s.scene * grid.rowHeight();
        if (y < p.y)
            p.y = y;
        else if (y + grid.rowHeight() > p.y + slots.getHeight())
            p.y = y + grid.rowHeight() - slots.getHeight();
    }
    setScrollPosition(p);
}

juce::Rectangle<int> SessionView::slotOnScreen(int column, int scene) const
{
    return grid.slotBounds(column, scene).translated(slots.getX() - scroll.x, slots.getY() - scroll.y);
}

int SessionView::trackOf(int column) const
{
    if (column < 0 || column >= static_cast<int>(grid.columns().size()))
        return -1;
    const auto& c = grid.columns()[static_cast<std::size_t>(column)];
    return c.kind == SessionLayout::Column::Kind::track ? c.index : -1;
}

// --- Hits and actions ----------------------------------------------------------------------------------

SessionView::Hit SessionView::hitAt(juce::Point<int> p) const
{
    Hit hit;
    if (scenes.contains(p))
    {
        if (p.y >= slots.getY() && p.y < slots.getBottom())
        {
            hit.scene = grid.sceneAt(p.y - slots.getY() + scroll.y);
            if (hit.scene >= 0)
                hit.area = p.x < scenes.getX() + grid.rowHeight() ? Area::sceneLaunch : Area::scene;
        }
        else if (p.y >= stops.getY() && p.y < stops.getBottom())
            hit.area = Area::stopAll;
        return hit;
    }
    if (p.x >= slots.getRight())
        return hit;
    hit.column = grid.columnAt(p.x - slots.getX() + scroll.x);
    if (hit.column < 0)
        return hit;
    if (headers.contains(p))
    {
        const auto fold = grid.foldButton(hit.column).translated(slots.getX() - scroll.x, headers.getY());
        const auto& c = grid.columns()[static_cast<std::size_t>(hit.column)];
        hit.area = c.isGroup && fold.contains(p) ? Area::fold : Area::header;
    }
    else if (slots.contains(p))
    {
        hit.scene = grid.sceneAt(p.y - slots.getY() + scroll.y);
        if (hit.scene < 0)
            return {};
        const auto r = slotOnScreen(hit.column, hit.scene);
        hit.area = grid.launchButton(r).contains(p) ? Area::launch : Area::slot;
    }
    else if (stops.contains(p))
        hit.area = Area::stop;
    return hit;
}

void SessionView::press(const Hit& hit)
{
    auto& set = session.set();
    const int track = trackOf(hit.column);
    switch (hit.area)
    {
    case Area::fold:
        set.setGroupFolded(track, !static_cast<bool>(set.track(track)[ids::folded]));
        selection.set({Kind::track, track, selection.get().scene});
        break;
    case Area::header:
        if (track >= 0)
            selection.set({Kind::track, track, selection.get().scene});
        break;
    case Area::launch:
        if (track < 0)
            break;
        selection.set({Kind::slot, track, hit.scene});
        session.launchSlot(track, hit.scene);
        pressed = hit;
        break;
    case Area::slot:
        if (track >= 0)
            selection.set({Kind::slot, track, hit.scene});
        break;
    case Area::stop:
        if (track >= 0)
            session.stopTrack(track);
        break;
    case Area::sceneLaunch:
        session.launchScene(hit.scene);
        selection.set({Kind::scene, selection.get().track,
                       set.selectNextSceneOnLaunch() ? juce::jmin(hit.scene + 1, set.numScenes() - 1) : hit.scene});
        break;
    case Area::scene:
        selection.set({Kind::scene, selection.get().track, hit.scene});
        break;
    case Area::stopAll:
        session.stopAllClips();
        break;
    case Area::none:
        break;
    }
}

void SessionView::release()
{
    if (pressed.area == Area::launch)
        if (const int track = trackOf(pressed.column); track >= 0)
            session.releaseSlot(track, pressed.scene);
    pressed = {};
}

void SessionView::doubleClick(const Hit& hit)
{
    auto& set = session.set();
    const int track = trackOf(hit.column);
    if (hit.area == Area::header || hit.area == Area::scene)
    {
        renameSelection();
    }
    else if (hit.area == Area::slot && track >= 0 && !set.isGroupTrack(track) && !set.clip(track, hit.scene).isValid())
    {
        selection.set({Kind::slot, track, hit.scene});
        commands.perform("create.midiClip");
    }
}

void SessionView::renameSelection()
{
    auto& set = session.set();
    const auto s = selection.get();
    juce::ValueTree node;
    juce::String what;
    switch (s.kind)
    {
    case Kind::track:
        node = set.track(s.track);
        what = "Rename track";
        break;
    case Kind::scene:
        node = set.scene(s.scene);
        what = "Rename scene";
        break;
    case Kind::slot:
        node = set.clip(s.track, s.scene);
        what = "Rename clip";
        break;
    }
    if (!node.isValid())
        return;
    askForText(*this, what, node[ids::name].toString(), [this, s, what](const juce::String& text) {
        auto& liveSet = session.set();
        session.undoManager().beginNewTransaction(what);
        if (s.kind == Kind::track)
            liveSet.setTrackName(s.track, text);
        else if (s.kind == Kind::scene)
            liveSet.setSceneName(s.scene, text);
        else if (auto clip = liveSet.clip(s.track, s.scene); clip.isValid())
            clip.setProperty(ids::name, text, liveSet.undoManager());
    });
}

void SessionView::showMenu(const Hit& hit)
{
    auto& manager = commands.manager();
    const auto add = [&](juce::PopupMenu& menu, const char* id) { menu.addCommandItem(&manager, commands.numericIdOf(id)); };
    juce::PopupMenu menu;
    const int track = trackOf(hit.column);
    switch (hit.area)
    {
    case Area::launch:
    case Area::slot:
    {
        if (track < 0)
            return;
        selection.set({Kind::slot, track, hit.scene});
        for (const auto* id : {"create.midiClip", "file.importMidi", "edit.duplicate", "edit.delete", "edit.rename"})
            add(menu, id);
        menu.addSeparator();
        auto& set = session.set();
        const bool has = set.hasStopButton(track, hit.scene);
        menu.addItem("Stop Button", !set.clip(track, hit.scene).isValid(), has, [this, track, scene = hit.scene, has] {
            session.undoManager().beginNewTransaction("Stop button");
            session.set().setHasStopButton(track, scene, !has);
        });
        break;
    }
    case Area::header:
    case Area::fold:
        if (track < 0)
            return;
        selection.set({Kind::track, track, selection.get().scene});
        for (const auto* id : {"create.midiTrack", "create.group", "edit.ungroup", "edit.duplicate", "edit.delete", "edit.rename"})
            add(menu, id);
        break;
    case Area::scene:
    case Area::sceneLaunch:
        selection.set({Kind::scene, selection.get().track, hit.scene});
        for (const auto* id : {"session.launch", "create.scene", "edit.duplicate", "edit.delete", "edit.rename"})
            add(menu, id);
        break;
    default:
        return;
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
}

void SessionView::mouseDown(const juce::MouseEvent& e)
{
    const auto hit = hitAt(e.getPosition());
    if (e.mods.isPopupMenu())
        showMenu(hit);
    else
        press(hit);
}

void SessionView::mouseUp(const juce::MouseEvent&)
{
    release();
}

void SessionView::mouseDoubleClick(const juce::MouseEvent& e)
{
    if (!e.mods.isPopupMenu())
        doubleClick(hitAt(e.getPosition()));
}

void SessionView::mouseWheelMove(const juce::MouseEvent& e, const juce::MouseWheelDetails& wheel)
{
    const float step = static_cast<float>(grid.rowHeight()) * 3.0f;
    const bool sideways = e.mods.isShiftDown() || std::abs(wheel.deltaX) > std::abs(wheel.deltaY);
    const float amount = (sideways && !e.mods.isShiftDown() ? wheel.deltaX : wheel.deltaY) * (wheel.isReversed ? 1.0f : -1.0f);
    if (sideways)
        setScrollPosition({scroll.x + juce::roundToInt(amount * step * 4.0f), scroll.y});
    else
        setScrollPosition({scroll.x, scroll.y + juce::roundToInt(amount * step * 4.0f)});
}

// --- Listening -------------------------------------------------------------------------------------------

void SessionView::engineStateChanged(std::uint32_t aspects)
{
    const int numColumns = static_cast<int>(grid.columns().size());
    if ((aspects & EngineStateFeed::metersAspect) != 0)
    {
        for (int c = 0; c < static_cast<int>(strips.size()) && c < numColumns; ++c)
        {
            const auto& col = grid.columns()[static_cast<std::size_t>(c)];
            strips[static_cast<std::size_t>(c)]->showLevels(
                col.kind == SessionLayout::Column::Kind::track ? feed.trackPeaks(col.index) : feed.returnPeaks(col.index),
                feed.frameSeconds());
        }
        if (master != nullptr)
            master->showLevels(feed.masterPeaks(), feed.frameSeconds());
        if (aspects == EngineStateFeed::metersAspect)
            return;
    }
    if ((aspects & EngineStateFeed::slotsAspect) != 0)
        for (const auto& [track, scene] : feed.changedSlots())
            if (const int c = grid.columnOfTrack(track); c >= 0)
                repaint(slotOnScreen(c, scene));
    // Every frame: the playing slots' progress; the queued ones' blinking.
    for (int c = 0; c < numColumns; ++c)
    {
        const int track = trackOf(c);
        if (track < 0)
            continue;
        for (int s = 0; s < grid.numScenes(); ++s)
        {
            const auto state = feed.slot(track, s);
            if (state == SlotState::Playing || state == SlotState::Recording || state == SlotState::QueuedLaunch ||
                state == SlotState::QueuedStop || state == SlotState::QueuedRecord)
                repaint(grid.launchButton(slotOnScreen(c, s)).getIntersection(slots));
        }
    }
}

void SessionView::selectionChanged(const SessionSelection&)
{
    repaint();
    revealSelection();
}

namespace
{
/** Changes that move columns or rows: the layout and the strips are built again. */
bool changesStructure(const juce::ValueTree& parent)
{
    return parent.hasType(ids::tracks) || parent.hasType(ids::returns) || parent.hasType(ids::scenes) ||
           parent.hasType(ids::liveSet);
}
} // namespace

void SessionView::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    if (insideDevice(tree))
        return;
    if (property == ids::folded || property == ids::groupId || property == ids::trackKind)
        structureChanged = true;
    triggerAsyncUpdate();
}

void SessionView::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree&)
{
    if (insideDevice(parent))
        return;
    if (changesStructure(parent))
        structureChanged = true;
    triggerAsyncUpdate();
}

void SessionView::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int)
{
    if (insideDevice(parent))
        return;
    if (changesStructure(parent))
        structureChanged = true;
    triggerAsyncUpdate();
}

void SessionView::handleAsyncUpdate()
{
    if (structureChanged)
        rebuild();
    else
        repaint(); // names, colours, clips, arm...: the strips follow the document by themselves
}

bool SessionView::blinkOn() const
{
    return std::fmod(feed.now() * 2.0, 1.0) < 0.5;
}

// --- Painting --------------------------------------------------------------------------------------------

void SessionView::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    g.fillAll(t.background);
    {
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(headers);
        paintHeaders(g);
    }
    {
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(slots);
        paintSlots(g);
    }
    {
        juce::Graphics::ScopedSaveState state(g);
        g.reduceClipRegion(stops);
        paintStops(g);
    }
    paintScenes(g);
    if (showMixer)
    {
        g.setColour(t.border);
        g.fillRect(mixer.withHeight(1));
    }
}

void SessionView::paintHeaders(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    auto& set = session.set();
    const auto sel = selection.get();
    for (int c = 0; c < static_cast<int>(grid.columns().size()); ++c)
    {
        const auto& col = grid.columns()[static_cast<std::size_t>(c)];
        auto r = juce::Rectangle<int>(slots.getX() + col.x - scroll.x, headers.getY(), col.width, headers.getHeight());
        if (r.getRight() < headers.getX() || r.getX() > headers.getRight())
            continue;
        const bool isTrack = col.kind == SessionLayout::Column::Kind::track;
        const auto node = isTrack ? set.track(col.index) : set.returnTrack(col.index);
        const bool selected = isTrack && sel.kind == Kind::track && sel.track == col.index;
        g.setColour(selected ? t.surfaceRaised.brighter(0.1f) : col.isGroup ? t.surfaceRaised : t.surface);
        g.fillRect(r.reduced(1, 0));
        g.setColour(colourOf(node));
        g.fillRect(r.reduced(1, 0).removeFromTop(3));
        auto text = r.reduced(juce::roundToInt(t.padding), 0).withTrimmedTop(3);
        text.removeFromLeft(juce::roundToInt(t.spacing * 2.0f) * col.depth); // inside groups: indented
        if (col.isGroup)
        {
            const auto fold = grid.foldButton(c).translated(slots.getX() - scroll.x, headers.getY()).toFloat().reduced(t.padding);
            g.setColour(t.textDim);
            juce::Path p;
            if (col.folded)
                p.addTriangle(fold.getX(), fold.getY(), fold.getX(), fold.getBottom(), fold.getRight(), fold.getCentreY());
            else
                p.addTriangle(fold.getX(), fold.getY(), fold.getRight(), fold.getY(), fold.getCentreX(), fold.getBottom());
            g.fillPath(p);
            text.removeFromLeft(juce::roundToInt(fold.getWidth() + t.spacing));
        }
        g.setColour(t.text);
        g.setFont(selected ? juce::Font(t.font(t.fontNormal)).boldened() : juce::Font(t.font(t.fontNormal)));
        g.drawFittedText(node[ids::name].toString(), text, juce::Justification::centredLeft, 1, 0.8f);
        if (selected)
        {
            g.setColour(t.selection);
            g.drawRect(r.reduced(1, 0), 1);
        }
    }
}

void SessionView::paintSlots(juce::Graphics& g)
{
    const int first = juce::jmax(0, grid.sceneAt(scroll.y));
    const int rowH = juce::jmax(1, grid.rowHeight());
    const int last = juce::jmin(grid.numScenes() - 1, (scroll.y + slots.getHeight()) / rowH);
    for (int c = 0; c < static_cast<int>(grid.columns().size()); ++c)
    {
        const auto& col = grid.columns()[static_cast<std::size_t>(c)];
        if (col.x + col.width < scroll.x || col.x > scroll.x + slots.getWidth())
            continue;
        for (int s = first; s <= last; ++s)
            paintSlot(g, c, s, slotOnScreen(c, s));
    }
}

void SessionView::paintSlot(juce::Graphics& g, int column, int scene, juce::Rectangle<int> r)
{
    const auto& t = themeOf(*this);
    const auto& col = grid.columns()[static_cast<std::size_t>(column)];
    auto cell = r.reduced(1).toFloat();
    if (col.kind == SessionLayout::Column::Kind::returnTrack)
    {
        g.setColour(t.surfaceSunken.withMultipliedAlpha(0.5f));
        g.fillRect(cell);
        return;
    }
    auto& set = session.set();
    const int track = col.index;
    const auto state = feed.slot(track, scene);
    const auto clip = set.clip(track, scene);
    const bool group = col.isGroup;
    const bool hasContent = clip.isValid() || (group && state != SlotState::Empty);

    // The cell.
    if (hasContent)
    {
        const auto colour = group ? t.surfaceRaised.brighter(0.15f) : colourOf(clip).interpolatedWith(t.surface, 0.25f);
        g.setColour(colour);
        g.fillRoundedRectangle(cell, t.radius);
        if (!group && clip.isValid())
        {
            g.setColour(textOn(colour, t));
            g.setFont(t.font(t.fontSmall));
            g.drawFittedText(clip[ids::name].toString(), r.withTrimmedLeft(r.getHeight()).reduced(2, 0),
                             juce::Justification::centredLeft, 1, 0.8f);
        }
    }
    else
    {
        g.setColour(t.surfaceSunken);
        g.fillRoundedRectangle(cell, t.radius);
    }

    // The launch button: play, stop or record.
    const auto button = grid.launchButton(r).toFloat().reduced(static_cast<float>(r.getHeight()) * 0.3f);
    const bool blink = blinkOn();
    const bool armed = !group && static_cast<bool>(set.track(track)[ids::arm]);
    if (hasContent)
    {
        switch (state)
        {
        case SlotState::Recording:
            g.setColour(t.record);
            g.fillEllipse(button);
            break;
        case SlotState::QueuedRecord:
            g.setColour(blink ? t.record : t.textDim);
            g.fillEllipse(button);
            break;
        case SlotState::Playing:
        case SlotState::QueuedStop:
        {
            g.setColour(state == SlotState::QueuedStop && blink ? t.textDim : t.play);
            triangle(g, button);
            // Progress round the button: where the playing clip is in its loop.
            if (!group)
            {
                const auto& play = feed.trackPlay(track);
                const double length = play.loopEnd - play.loopStart;
                const float done = length > 0.0 ? static_cast<float>(juce::jlimit(0.0, 1.0, (play.position - play.loopStart) / length)) : 0.0f;
                juce::Path ring;
                const auto c = button.getCentre();
                const float radius = button.getWidth() * 0.95f;
                ring.addCentredArc(c.x, c.y, radius, radius, 0.0f, 0.0f, juce::MathConstants<float>::twoPi * done, true);
                g.strokePath(ring, juce::PathStrokeType(1.5f));
            }
            break;
        }
        case SlotState::QueuedLaunch:
            g.setColour(blink ? t.play : t.textDim);
            triangle(g, button);
            break;
        case SlotState::Stopped:
        case SlotState::Empty:
            g.setColour(textOn(group ? t.surfaceRaised : colourOf(clip), t).withMultipliedAlpha(0.8f));
            triangle(g, button);
            break;
        }
    }
    else if (armed)
    {
        g.setColour(t.record.withMultipliedAlpha(0.7f));
        g.drawEllipse(button, 1.5f);
    }
    else if (set.hasStopButton(track, scene))
    {
        g.setColour(t.textDisabled);
        g.fillRect(button.reduced(button.getWidth() * 0.15f));
    }

    const auto sel = selection.get();
    if (sel.kind == Kind::slot && sel.track == track && sel.scene == scene)
    {
        g.setColour(t.selection);
        g.drawRoundedRectangle(cell, t.radius, 1.5f);
    }
}

void SessionView::paintStops(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    g.setColour(t.surface);
    g.fillRect(stops);
    for (int c = 0; c < static_cast<int>(grid.columns().size()); ++c)
    {
        const auto& col = grid.columns()[static_cast<std::size_t>(c)];
        if (col.kind != SessionLayout::Column::Kind::track)
            continue;
        const auto r = juce::Rectangle<int>(slots.getX() + col.x - scroll.x, stops.getY(), col.width, stops.getHeight());
        const auto button = r.withSizeKeepingCentre(r.getHeight(), r.getHeight()).toFloat().reduced(static_cast<float>(r.getHeight()) * 0.32f);
        g.setColour(t.textDim);
        g.fillRect(button);
    }
}

void SessionView::paintScenes(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    auto& set = session.set();
    g.setColour(t.surface);
    g.fillRect(scenes);
    // The corner above: "Master".
    const auto corner = juce::Rectangle<int>(scenes.getX(), headers.getY(), scenes.getWidth(), headers.getHeight());
    g.setColour(t.textDim);
    g.setFont(juce::Font(t.font(t.fontNormal)).boldened());
    g.drawFittedText("Master", corner.reduced(juce::roundToInt(t.padding), 0), juce::Justification::centredLeft, 1);
    // Stop All, under the scenes.
    const auto stopAll = juce::Rectangle<int>(scenes.getX(), stops.getY(), scenes.getWidth(), stops.getHeight()).reduced(2);
    g.setColour(t.surfaceRaised);
    g.fillRoundedRectangle(stopAll.toFloat(), t.radius);
    g.setColour(t.text);
    g.setFont(t.font(t.fontSmall));
    g.drawFittedText("Stop All Clips", stopAll, juce::Justification::centred, 1);

    // The scenes, scrolling with the slots.
    juce::Graphics::ScopedSaveState state(g);
    g.reduceClipRegion(scenes.withY(slots.getY()).withHeight(slots.getHeight()));
    const auto sel = selection.get();
    for (int s = 0; s < set.numScenes(); ++s)
    {
        const auto r = juce::Rectangle<int>(scenes.getX(), slots.getY() + s * grid.rowHeight() - scroll.y, scenes.getWidth(), grid.rowHeight());
        if (r.getBottom() < slots.getY() || r.getY() > slots.getBottom())
            continue;
        const auto scene = set.scene(s);
        g.setColour(t.surfaceRaised);
        g.fillRoundedRectangle(r.reduced(1).toFloat(), t.radius);
        const auto button = r.withWidth(r.getHeight()).toFloat().reduced(static_cast<float>(r.getHeight()) * 0.3f);
        g.setColour(t.text);
        triangle(g, button);
        juce::String label = scene[ids::name].toString();
        if (label.isEmpty())
            label = juce::String(s + 1);
        const double tempo = scene[ids::sceneTempo];
        const int num = scene[ids::sceneTimeSigNum], den = scene[ids::sceneTimeSigDen];
        juce::String extra;
        if (tempo > 0.0)
            extra << juce::String(tempo, 0) << " BPM";
        if (num > 0 && den > 0)
            extra << (extra.isEmpty() ? "" : " ") << num << "/" << den;
        g.setFont(t.font(t.fontSmall));
        auto text = r.withTrimmedLeft(r.getHeight()).reduced(2, 0);
        if (extra.isNotEmpty())
        {
            g.setColour(t.textDim);
            g.drawFittedText(extra, text, juce::Justification::centredRight, 1, 0.8f);
        }
        g.setColour(t.text);
        g.drawFittedText(label, text, juce::Justification::centredLeft, 1, 0.8f);
        if (sel.kind == Kind::scene && sel.scene == s)
        {
            g.setColour(t.selection);
            g.drawRoundedRectangle(r.reduced(1).toFloat(), t.radius, 1.5f);
        }
    }
}

} // namespace lattice::ui
