#include "ui/mapping/MapOverlay.h"

#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui
{
namespace
{
const juce::Identifier targetId{"target"}, nameId{"name"}, minId{"min"}, maxId{"max"}, takeoverId{"takeover"}, encoderId{"encoder"};
const juce::String arrow = juce::String::fromUTF8(" \xe2\x80\xba "); // a small right angle bracket
}

MapOverlay::MapOverlay(Mappings& m, ControlRegistry& c, SessionView& s) : mappings(m), controls(c), sessionView(s)
{
    setTitle("Map mode");
    setWantsKeyboardFocus(true);
    setInterceptsMouseClicks(true, false);
}

juce::Rectangle<int> MapOverlay::boundsOf(const juce::Component& c) const
{
    return getLocalArea(&c, c.getLocalBounds());
}

std::pair<juce::String, juce::String> MapOverlay::targetAt(juce::Point<int> local) const
{
    // The smallest control under the point (a knob inside a panel, not the panel).
    const ControlRegistry::Entry* best = nullptr;
    int bestArea = std::numeric_limits<int>::max();
    for (const auto* e : controls.entries())
    {
        if (e->binding == nullptr || e->binding->target.isEmpty() || e->component == nullptr || !e->component->isShowing())
            continue;
        const auto r = boundsOf(*e->component);
        if (r.contains(local) && r.getWidth() * r.getHeight() < bestArea)
        {
            best = e;
            bestArea = r.getWidth() * r.getHeight();
        }
    }
    if (best != nullptr)
        return {best->binding->target, best->owner.isNotEmpty() ? best->owner + arrow + best->name : best->name};

    // A clip slot or a scene in the Session view.
    if (!sessionView.isShowing())
        return {};
    const auto inSession = sessionView.getLocalPoint(this, local);
    if (!sessionView.getLocalBounds().contains(inSession))
        return {};
    const auto hit = sessionView.hitAt(inSession);
    auto& set = sessionView.sessionController().set();
    const auto& columns = sessionView.layout().columns();
    if ((hit.area == SessionView::Hit::Area::slot || hit.area == SessionView::Hit::Area::launch) && hit.column >= 0 &&
        hit.column < static_cast<int>(columns.size()) && columns[static_cast<std::size_t>(hit.column)].kind == SessionLayout::Column::Kind::track)
    {
        const int track = columns[static_cast<std::size_t>(hit.column)].index;
        return {"slot:" + juce::String(set.trackId(track)) + "/" + juce::String(hit.scene),
                set.track(track)[ids::name].toString() + arrow + "Slot " + juce::String(hit.scene + 1)};
    }
    if ((hit.area == SessionView::Hit::Area::scene || hit.area == SessionView::Hit::Area::sceneLaunch) && hit.scene >= 0)
    {
        const auto scene = set.scene(hit.scene);
        const auto name = scene[ids::name].toString();
        return {"scene:" + juce::String(LiveSet::idOf(scene)), "Scene " + (name.isNotEmpty() ? name : juce::String(hit.scene + 1))};
    }
    return {};
}

void MapOverlay::choose(juce::Point<int> local)
{
    const auto [target, name] = targetAt(local);
    if (target.isNotEmpty())
        mappings.setLearnTarget(target, name);
    repaint();
}

void MapOverlay::mouseDown(const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    choose(e.getPosition());
}

void MapOverlay::mouseMove(const juce::MouseEvent& e)
{
    hover = e.getPosition();
    repaint();
}

bool MapOverlay::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey)
    {
        if (onExit)
            onExit();
        return true;
    }
    if (passesThrough && passesThrough(key))
        return false;
    if (mappings.mode() == Mappings::Mode::key)
        return mappings.handleKey(key);
    return false; // MIDI Map: keys keep doing what they do (Ctrl+M leaves)
}

void MapOverlay::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    const bool keyMode = mappings.mode() == Mappings::Mode::key;
    const auto tint = keyMode ? t.queued : t.accent;
    g.setFont(t.font(t.fontSmall));
    const auto label = [&](juce::Rectangle<int> r, const juce::String& target) {
        juce::StringArray sources;
        for (const auto& m : mappings.forTarget(target))
            sources.add(Mappings::sourceText(m).upToFirstOccurrenceOf(juce::String::fromUTF8(" \xc2\xb7"), false, false));
        if (sources.isEmpty())
            return;
        const auto text = sources.joinIntoString(", ");
        auto badge = r.withHeight(juce::roundToInt(t.fontSmall + 4.0f)).withWidth(juce::jmax(r.getWidth(), juce::roundToInt(t.fontSmall * 0.6f * static_cast<float>(text.length())) + 6));
        g.setColour(tint);
        g.fillRect(badge);
        g.setColour(t.accentText);
        g.drawText(text, badge, juce::Justification::centred, true);
    };
    for (const auto* e : controls.entries())
    {
        if (e->binding == nullptr || e->binding->target.isEmpty() || e->component == nullptr || !e->component->isShowing())
            continue;
        const auto r = boundsOf(*e->component);
        if (!getLocalBounds().intersects(r))
            continue;
        g.setColour(tint.withAlpha(0.22f));
        g.fillRect(r);
        if (e->binding->target == mappings.learnTarget())
        {
            g.setColour(tint);
            g.drawRect(r, 2);
        }
        label(r, e->binding->target);
    }
    // Slots and scenes: the one under the pointer, the one chosen, and those mapped.
    if (sessionView.isShowing())
    {
        const auto [hovered, hoveredName] = targetAt(hover);
        juce::ignoreUnused(hoveredName);
        const auto& columns = sessionView.layout().columns();
        auto& set = sessionView.sessionController().set();
        for (int c = 0; c < static_cast<int>(columns.size()); ++c)
        {
            if (columns[static_cast<std::size_t>(c)].kind != SessionLayout::Column::Kind::track)
                continue;
            const auto trackId = set.trackId(columns[static_cast<std::size_t>(c)].index);
            for (int s = 0; s < set.numScenes(); ++s)
            {
                const auto target = "slot:" + juce::String(trackId) + "/" + juce::String(s);
                const bool mapped = !mappings.forTarget(target).empty();
                if (!mapped && target != hovered && target != mappings.learnTarget())
                    continue;
                const auto r = getLocalArea(&sessionView, sessionView.slotOnScreen(c, s));
                g.setColour(tint.withAlpha(0.3f));
                g.fillRect(r);
                if (target == mappings.learnTarget())
                {
                    g.setColour(tint);
                    g.drawRect(r, 2);
                }
                label(r, target);
            }
        }
    }
    // What to do next, along the top.
    const auto hint = mappings.learnTarget().isEmpty()
                          ? juce::String(keyMode ? "Key Map: click a control, a clip slot or a scene, then press a key. Esc leaves."
                                                 : "MIDI Map: click a control, a clip slot or a scene, then move a knob or press a pad. Ctrl+M leaves.")
                          : juce::String(keyMode ? "Press a key for it." : "Move a knob or press a pad for it.");
    auto top = getLocalBounds().removeFromTop(juce::roundToInt(t.fontNormal + t.spacing * 2.0f));
    g.setColour(tint.withAlpha(0.9f));
    g.fillRect(top);
    g.setColour(t.accentText);
    g.setFont(t.font(t.fontNormal));
    g.drawText(hint, top.reduced(juce::roundToInt(t.padding), 0), juce::Justification::centredLeft, true);
}

// --- MappingsView --------------------------------------------------------------------------------

MappingsView::MappingsView(Mappings& m, CommandRegistry& c) : mappings(m), commands(c)
{
    setTitle("Mappings");
    mapCommand.setTooltip("Chooses a command (Play, Undo, Show Browser...) as what the next note, CC or key maps to.");
    mapCommand.onClick = [this] {
        // The commands by category, as in the menu bar.
        std::map<juce::String, juce::PopupMenu> byCategory;
        for (const auto* command : commands.all())
            byCategory[command->category].addItem(command->name, [this, id = command->id, name = command->name] {
                mappings.setLearnTarget("command:" + id, name);
            });
        juce::PopupMenu menu;
        for (auto& [category, sub] : byCategory)
            menu.addSubMenu(category, sub);
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&mapCommand));
    };
    addAndMakeVisible(mapCommand);
    refresh();
}

void MappingsView::refresh()
{
    rows = mappings.all();
    repaint();
}

int MappingsView::rowHeight() const
{
    return juce::roundToInt(themeOf(*this).fontNormal * 2.0f + themeOf(*this).spacing * 2.0f);
}

void MappingsView::resized()
{
    const auto& t = themeOf(*this);
    auto area = getLocalBounds().reduced(juce::roundToInt(t.padding));
    headingArea = area.removeFromTop(juce::roundToInt(t.fontNormal * 3.0f + t.spacing * 2.0f));
    mapCommand.setBounds(area.removeFromTop(juce::roundToInt(t.controlHeight)));
    area.removeFromTop(juce::roundToInt(t.spacing * 2.0f));
    listArea = area;
}

void MappingsView::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    g.fillAll(t.surface);
    g.setColour(t.text);
    g.setFont(juce::Font(t.font(t.fontNormal)).boldened());
    const bool keyMode = mappings.mode() == Mappings::Mode::key;
    auto heading = headingArea;
    g.drawText(keyMode ? "Key Map" : "MIDI Map", heading.removeFromTop(juce::roundToInt(t.fontNormal + t.spacing)), juce::Justification::centredLeft, true);
    g.setFont(t.font(t.fontSmall));
    g.setColour(t.textDim);
    g.drawFittedText(mappings.learnTarget().isEmpty() ? juce::String("Click what to map.")
                                                      : (keyMode ? juce::String("Press a key for the chosen control.") : juce::String("Move a MIDI control for the chosen control.")),
                     heading, juce::Justification::topLeft, 2);

    g.saveState();
    g.reduceClipRegion(listArea);
    const int h = rowHeight();
    auto row = listArea.withHeight(h);
    if (rows.empty())
    {
        g.setColour(t.textDim);
        g.drawFittedText("Nothing is mapped yet.", row, juce::Justification::centredLeft, 1);
    }
    for (const auto& m : rows)
    {
        auto r = row.reduced(0, juce::roundToInt(t.spacing * 0.5f));
        g.setColour(m[targetId].toString() == mappings.learnTarget() ? t.selection.withAlpha(0.4f) : t.surfaceRaised);
        g.fillRect(r);
        r = r.reduced(juce::roundToInt(t.padding * 0.5f), 0);
        g.setColour(t.text);
        g.setFont(t.font(t.fontNormal));
        g.drawText(m[nameId].toString(), r.removeFromTop(r.getHeight() / 2), juce::Justification::centredLeft, true);
        g.setColour(t.textDim);
        g.setFont(t.font(t.fontSmall));
        juce::String detail = Mappings::sourceText(m);
        const double lo = m[minId], hi = m[maxId];
        if (lo != 0.0 || hi != 1.0)
            detail << "  " << juce::String(juce::roundToInt(lo * 100.0)) << "-" << juce::String(juce::roundToInt(hi * 100.0)) << "%";
        g.drawText(detail, r, juce::Justification::centredLeft, true);
        row.translate(0, h);
    }
    g.restoreState();
}

juce::PopupMenu MappingsView::menuFor(const juce::ValueTree& mapping)
{
    juce::PopupMenu menu;
    const auto takeover = static_cast<Mappings::Takeover>(static_cast<int>(mapping[takeoverId]));
    const auto encoder = static_cast<Mappings::Encoder>(static_cast<int>(mapping[encoderId]));
    const double lo = mapping[minId], hi = mapping[maxId];
    juce::PopupMenu range;
    for (const auto& [name, a, b] : {std::tuple{"Full (0-100%)", 0.0, 1.0}, std::tuple{"Lower half (0-50%)", 0.0, 0.5},
                                     std::tuple{"Upper half (50-100%)", 0.5, 1.0}, std::tuple{"Reversed (100-0%)", 1.0, 0.0}})
        range.addItem(name, true, lo == a && hi == b, [this, mapping, a = a, b = b] { mappings.setRange(mapping, a, b); });
    menu.addSubMenu("Range", range);
    juce::PopupMenu take;
    take.addItem("Pickup", true, takeover == Mappings::Takeover::pickup, [this, mapping] { mappings.setTakeover(mapping, Mappings::Takeover::pickup); });
    take.addItem("Value Scaling", true, takeover == Mappings::Takeover::scaling,
                 [this, mapping] { mappings.setTakeover(mapping, Mappings::Takeover::scaling); });
    menu.addSubMenu("Takeover", take, !mapping[juce::Identifier("note")]);
    juce::PopupMenu enc;
    for (const auto& [name, e] : {std::pair{"Absolute", Mappings::Encoder::absolute}, std::pair{"Relative (Two's Complement)", Mappings::Encoder::twosComplement},
                                  std::pair{"Relative (Binary Offset)", Mappings::Encoder::binaryOffset},
                                  std::pair{"Relative (Sign Magnitude)", Mappings::Encoder::signMagnitude}})
        enc.addItem(name, true, encoder == e, [this, mapping, e = e] { mappings.setEncoder(mapping, e); });
    menu.addSubMenu("Encoder", enc, !mapping[juce::Identifier("note")] && mapping[juce::Identifier("kind")].toString() == "midi");
    menu.addSeparator();
    menu.addItem("Remove", [this, mapping] { mappings.remove(mapping); });
    return menu;
}

void MappingsView::mouseDown(const juce::MouseEvent& e)
{
    if (!listArea.contains(e.getPosition()))
        return;
    const int i = (e.y - listArea.getY()) / rowHeight();
    if (i < 0 || i >= static_cast<int>(rows.size()))
        return;
    const auto mapping = rows[static_cast<std::size_t>(i)];
    if (e.mods.isPopupMenu())
        menuFor(mapping).showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
    else
        mappings.setLearnTarget(mapping[targetId].toString(), mapping[nameId].toString()); // remap it
}

} // namespace lattice::ui
