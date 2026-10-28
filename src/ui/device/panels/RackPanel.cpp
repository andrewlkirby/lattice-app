// The racks' panel (docs/05-m4-ui.md, 4d, "Racks"): Instrument Rack, Audio Effect Rack, Drum Rack.
// - Macros: any number (0-64), in rows that wrap to fit; names and colours; variations; map mode,
//   where clicking a parameter inside the rack maps the macro to it, and each mapping's range is
//   edited inline.
// - Chains: volume, pan, mute and solo per chain; selecting one shows its devices inline, and new
//   devices go there.
// - Zones (Instrument Racks): key and velocity zones as bars to drag, with fade handles.
// - Pads (Drum Racks): a grid whose page grows with the space; per-pad choke, tune, mute and solo;
//   samples dropped on pads; a pad dragged to another moves or swaps it.

#include "devices/racks/Rack.h"
#include "devices/racks/RackEditing.h"
#include "ui/device/DevicePanels.h"
#include "ui/device/DeviceView.h"
#include "ui/device/RackMath.h"
#include "ui/device/panels/PanelKit.h"
#include "ui/session/KeyboardFollow.h"
#include "ui/session/SessionView.h"
#include "ui/theme/LatticeLookAndFeel.h"
#include "ui/widgets/Icons.h"

#include <cmath>

namespace lattice::ui::panels
{
namespace
{
const juce::Identifier macroCountId{"macroCount"};
const juce::Identifier tunedPadsId{"tunedPads"};

juce::String panText(float v)
{
    const int amount = juce::roundToInt(std::abs(v) * 50.0f);
    return amount == 0 ? juce::String("C") : juce::String(amount) + (v < 0.0f ? "L" : "R");
}

std::optional<float> parsePan(const juce::String& text)
{
    const auto t = text.trim().toUpperCase();
    if (t == "C" || t.isEmpty())
        return 0.0f;
    const float amount = t.getFloatValue() / 50.0f;
    if (t.endsWithChar('L'))
        return -amount;
    if (t.endsWithChar('R'))
        return amount;
    return juce::jlimit(-1.0f, 1.0f, t.getFloatValue());
}

/** A binding to a property of a rack chain (its mix, its choke group, its play note). */
std::unique_ptr<ValueBinding> chainValue(DeviceContext& c, juce::ValueTree chain, ParameterSpec spec, juce::Identifier id)
{
    auto* set = &c.set();
    const bool flag = spec.kind == ParamKind::Bool;
    const bool whole = spec.kind != ParamKind::Float;
    return ValueBinding::property(std::move(spec), c.set().undoManager(), chain, id, [set, chain, id, flag, whole](double v) mutable {
        if (flag)
            chain.setProperty(id, v > 0.5, set->undoManager());
        else if (whole)
            chain.setProperty(id, juce::roundToInt(v), set->undoManager());
        else
            chain.setProperty(id, v, set->undoManager());
    });
}

ParameterSpec chainVolumeSpec()
{
    return ParameterSpec::continuous("chain.volume", "Chain Volume", static_cast<float>(LiveSet::minVolumeDb),
                                     static_cast<float>(LiveSet::maxVolumeDb), 0.0f, ParamUnit::Decibels)
        .withCentre(-12.0f)
        .inGroup("Chain");
}

ParameterSpec chainPanSpec()
{
    return ParameterSpec::continuous("chain.pan", "Chain Pan", -1.0f, 1.0f, 0.0f).inGroup("Chain").withFormatter(panText).withParser(parsePan);
}

int trackIndexOf(DeviceContext& c, const juce::ValueTree& node)
{
    const auto owner = LiveSet::chainOwnerOf(node);
    return owner.hasType(ids::track) ? c.set().trackIndexForId(LiveSet::idOf(owner)) : -1;
}

// --- Macros --------------------------------------------------------------------------------------------

class MacroGrid final : public juce::Component
{
public:
    MacroGrid(DeviceContext& c, juce::ValueTree r) : context(c), rack(std::move(r))
    {
        setTitle("Macros");
        auto state = rack.getChildWithName(ids::deviceState);
        auto* set = &context.set();
        count = std::make_unique<NumberBox>(ValueBinding::property(
            ParameterSpec::integer("rack.macroCount", "Macros", 0, RackDevice::maxMacros, RackDevice::defaultMacros).inGroup("Rack"),
            context.set().undoManager(), state, macroCountId, [set, rack = rack](double v) { racks::setMacroCount(*set, rack, juce::roundToInt(v)); }));
        count->setTooltip("How many macros the rack shows (0 to 64). Fewer macros remove the mappings of those that go.");
        addAndMakeVisible(*count);
        variations.setWantsKeyboardFocus(false);
        variations.setTooltip("Stores the macros' values as a variation, or recalls one.");
        variations.onClick = [this] { variationsMenu().showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&variations)); };
        addAndMakeVisible(variations);
        map.setClickingTogglesState(false);
        map.onClick = [this] {
            if (context.view == nullptr)
                return;
            if (context.view->mapModeRack() == rack)
                context.view->setMapMode({}, -1);
            else
                context.view->setMapMode(rack, juce::jlimit(0, juce::jmax(0, racks::macroCount(rack) - 1), lastMacro));
        };
        addAndMakeVisible(map);
        rebuild();
    }

    void rebuild()
    {
        knobs.clear();
        const int n = racks::macroCount(rack);
        const auto* proto = context.specs.forType(rack[ids::deviceType].toString());
        for (int m = 0; m < n; ++m)
        {
            const auto* p = proto->findParameter("macro" + juce::String(m + 1));
            if (p == nullptr)
                continue;
            auto knob = std::make_unique<Knob>(ValueBinding::deviceParameter(context.set(), rack, p->spec()));
            knob->extendMenu = [this, m](juce::PopupMenu& menu) { addMacroItems(menu, m); };
            addAndMakeVisible(*knob);
            knobs.push_back(std::move(knob));
        }
        refreshNames();
        mapModeChanged();
        resized();
    }

    void refreshNames()
    {
        for (int m = 0; m < static_cast<int>(knobs.size()); ++m)
        {
            auto& k = *knobs[static_cast<std::size_t>(m)];
            k.setLabel(racks::macroName(rack, m));
            const auto argb = racks::macroColour(rack, m);
            k.setArcColour(argb != 0 ? juce::Colour(argb) : juce::Colour());
        }
        repaint();
    }

    /** In map mode the knobs pick the macro to map instead of turning. */
    void mapModeChanged()
    {
        const bool mapping = context.view != nullptr && context.view->mapModeRack() == rack;
        for (auto& k : knobs)
            k->setInterceptsMouseClicks(!mapping, !mapping);
        map.setToggleState(mapping, juce::dontSendNotification);
        if (mapping)
            lastMacro = context.view->mapModeMacro();
        repaint();
    }

    const std::vector<std::unique_ptr<Knob>>& macroKnobs() const noexcept { return knobs; }
    int headerHeight() const { return juce::roundToInt(themeOf(*this).controlHeight + themeOf(*this).spacing); }
    int knobHeight() const { return knobs.empty() ? 1 : knobs.front()->preferredSize().getHeight(); }
    int knobWidth() const { return knobs.empty() ? 1 : knobs.front()->preferredSize().getWidth(); }

    int preferredWidth(int height) const
    {
        const auto& t = themeOf(*this);
        const auto fit = rackmath::fitMacros(static_cast<int>(knobs.size()), height - headerHeight(), knobHeight());
        // At least wide enough for its header: the title, the count, Variations and Map.
        return juce::jmax(juce::roundToInt(t.controlHeight * 10.5f), fit.columns * knobWidth());
    }

    void resized() override
    {
        const auto& t = themeOf(*this);
        auto r = getLocalBounds();
        auto header = r.removeFromTop(juce::roundToInt(t.controlHeight));
        r.removeFromTop(juce::roundToInt(t.spacing));
        map.setBounds(header.removeFromRight(header.getHeight()));
        variations.setBounds(header.removeFromRight(juce::roundToInt(t.controlHeight * 3.0f)).reduced(1, 0));
        count->setBounds(header.removeFromRight(juce::roundToInt(t.controlHeight * 2.0f)).reduced(1, 0));
        const auto fit = rackmath::fitMacros(static_cast<int>(knobs.size()), r.getHeight(), knobHeight());
        for (int i = 0; i < static_cast<int>(knobs.size()); ++i)
        {
            const int row = i / juce::jmax(1, fit.columns), column = i % juce::jmax(1, fit.columns);
            knobs[static_cast<std::size_t>(i)]->setBounds(r.getX() + column * knobWidth(), r.getY() + row * knobHeight(), knobWidth(), knobHeight());
        }
    }

    void paint(juce::Graphics& g) override
    {
        const auto& t = themeOf(*this);
        g.setFont(juce::Font(t.font(t.fontSmall)).boldened());
        g.setColour(t.textDim);
        g.drawText("MACROS", getLocalBounds().removeFromTop(juce::roundToInt(t.controlHeight)), juce::Justification::centredLeft, false);
        if (knobs.empty())
        {
            g.setFont(t.font(t.fontSmall));
            g.drawFittedText("No macros: set how many with the box above.", getLocalBounds().withTrimmedTop(headerHeight()),
                             juce::Justification::centred, 3);
        }
    }

    void paintOverChildren(juce::Graphics& g) override
    {
        if (context.view == nullptr || context.view->mapModeRack() != rack)
            return;
        // Map mode: the macro waiting for parameters is outlined, and the others say "click".
        const auto& t = themeOf(*this);
        for (int m = 0; m < static_cast<int>(knobs.size()); ++m)
        {
            const auto r = knobs[static_cast<std::size_t>(m)]->getBounds().toFloat().reduced(1.0f);
            const bool on = m == context.view->mapModeMacro();
            g.setColour(t.accent.withAlpha(on ? 0.35f : 0.1f));
            g.fillRoundedRectangle(r, t.radius);
            if (on)
            {
                g.setColour(t.accent);
                g.drawRoundedRectangle(r, t.radius, juce::jmax(2.0f, t.stroke * 2.0f));
            }
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (context.view == nullptr || context.view->mapModeRack() != rack)
            return;
        for (int m = 0; m < static_cast<int>(knobs.size()); ++m)
            if (knobs[static_cast<std::size_t>(m)]->getBounds().contains(e.getPosition()))
                context.view->setMapMode(rack, m);
    }

    IconButton& mapButton() noexcept { return map; }

private:
    juce::PopupMenu variationsMenu()
    {
        juce::PopupMenu m;
        juce::Component::SafePointer<MacroGrid> safe(this);
        m.addItem("Store Variation...", [safe] {
            if (safe == nullptr)
                return;
            askForText(*safe, "Store Variation", "Variation " + juce::String(racks::numVariations(safe->rack) + 1), [safe](const juce::String& name) {
                if (safe == nullptr)
                    return;
                safe->context.undo().beginNewTransaction("Store variation");
                racks::storeVariation(safe->context.set(), safe->rack, name);
            });
        });
        if (racks::numVariations(rack) > 0)
            m.addSeparator();
        for (int i = 0; i < racks::numVariations(rack); ++i)
            m.addItem("Recall " + racks::variationName(rack, i), [safe, i] {
                if (safe == nullptr)
                    return;
                safe->context.undo().beginNewTransaction("Recall variation");
                racks::recallVariation(safe->context.set(), safe->rack, i);
            });
        return m;
    }

    void addMacroItems(juce::PopupMenu& menu, int m)
    {
        juce::Component::SafePointer<MacroGrid> safe(this);
        menu.addSeparator();
        menu.addItem("Rename Macro...", [safe, m] {
            if (safe == nullptr)
                return;
            askForText(*safe, "Rename Macro", racks::macroName(safe->rack, m), [safe, m](const juce::String& name) {
                if (safe == nullptr)
                    return;
                safe->context.undo().beginNewTransaction("Rename macro");
                racks::setMacroName(safe->context.set(), safe->rack, m, name);
            });
        });
        juce::PopupMenu colours;
        colours.addItem("Default", [safe, m] {
            if (safe != nullptr)
            {
                safe->context.undo().beginNewTransaction("Macro colour");
                racks::setMacroColour(safe->context.set(), safe->rack, m, 0);
            }
        });
        for (int i = 0; i < LiveSet::paletteSize(); ++i)
        {
            const auto argb = LiveSet::paletteColour(i);
            juce::PopupMenu::Item item("Colour " + juce::String(i + 1));
            item.colour = juce::Colour(argb);
            item.action = [safe, m, argb] {
                if (safe == nullptr)
                    return;
                safe->context.undo().beginNewTransaction("Macro colour");
                racks::setMacroColour(safe->context.set(), safe->rack, m, argb);
            };
            colours.addItem(std::move(item));
        }
        menu.addSubMenu("Colour", colours);
        menu.addItem("Map Parameters...", [safe, m] {
            if (safe != nullptr && safe->context.view != nullptr)
                safe->context.view->setMapMode(safe->rack, m);
        });
        for (const auto& mapping : racks::mappingsOf(rack, m))
        {
            const auto target = context.set().deviceById(static_cast<juce::int64>(mapping[ids::targetDevice]));
            menu.addItem("Unmap " + context.nameOf(target) + " " + mapping[ids::parameter].toString(), [safe, mapping] {
                if (safe == nullptr)
                    return;
                safe->context.undo().beginNewTransaction("Remove mapping");
                racks::removeMapping(safe->context.set(), mapping);
            });
        }
    }

    DeviceContext& context;
    juce::ValueTree rack;
    std::unique_ptr<NumberBox> count;
    juce::TextButton variations{"Variations"};
    IconButton map{"Map", Icon::map, "Map mode: click a macro, then click parameters inside the rack to map them to it (again to unmap)."};
    std::vector<std::unique_ptr<Knob>> knobs;
    int lastMacro = 0;
};

/** The map mode macro's mappings, each with its range over the parameter's travel, edited inline. */
class MappingList final : public juce::Component
{
public:
    MappingList(DeviceContext& c, juce::ValueTree r) : context(c), rack(std::move(r)) { setTitle("Mappings"); }

    void rebuild()
    {
        rows.clear();
        const int macro = context.view != nullptr && context.view->mapModeRack() == rack ? context.view->mapModeMacro() : -1;
        shownMacro = macro;
        if (macro >= 0)
            for (const auto& mapping : racks::mappingsOf(rack, macro))
            {
                Row row;
                row.mapping = mapping;
                const auto target = context.set().deviceById(static_cast<juce::int64>(mapping[ids::targetDevice]));
                const auto* proto = context.specs.forType(target[ids::deviceType].toString());
                const auto* p = proto != nullptr ? proto->findParameter(mapping[ids::parameter].toString()) : nullptr;
                row.label = context.nameOf(target) + juce::String::fromUTF8(" \xe2\x80\xba ") + (p != nullptr ? p->spec().name : mapping[ids::parameter].toString());
                for (const auto& [id, name] : {std::pair{ids::minimum, "Min"}, std::pair{ids::maximum, "Max"}})
                {
                    // The ends as the parameter's own values ("200 Hz"), typed the same way.
                    auto spec = ParameterSpec::continuous("mapping." + id.toString(), name, 0.0f, 1.0f, id == ids::minimum ? 0.0f : 1.0f)
                                    .inGroup("Mapping");
                    if (p != nullptr)
                    {
                        const auto targetSpec = p->spec();
                        spec = std::move(spec)
                                   .withFormatter([targetSpec](float n) { return targetSpec.format(targetSpec.normalisableRange().convertFrom0to1(n)); })
                                   .withParser([targetSpec](const juce::String& text) -> std::optional<float> {
                                       return targetSpec.normalisableRange().convertTo0to1(targetSpec.parse(text));
                                   });
                    }
                    auto* set = &context.set();
                    auto box = std::make_unique<NumberBox>(ValueBinding::property(std::move(spec), context.set().undoManager(), mapping, id,
                                                                                  [set, node = juce::ValueTree(mapping), id](double v) mutable {
                                                                                      node.setProperty(id, juce::jlimit(0.0, 1.0, v), set->undoManager());
                                                                                  }));
                    addAndMakeVisible(*box);
                    row.boxes.push_back(std::move(box));
                }
                row.remove = std::make_unique<IconButton>("Unmap", Icon::cross, "Removes this mapping.");
                row.remove->onClick = [this, mapping] {
                    context.undo().beginNewTransaction("Remove mapping");
                    racks::removeMapping(context.set(), mapping);
                };
                addAndMakeVisible(*row.remove);
                rows.push_back(std::move(row));
            }
        resized();
        repaint();
    }

    bool isActive() const { return shownMacro >= 0; }
    int preferredWidth(int) const { return isActive() ? juce::roundToInt(themeOf(*this).controlHeight * 12.0f) : 0; }

    void resized() override
    {
        const auto& t = themeOf(*this);
        auto r = getLocalBounds();
        r.removeFromTop(juce::roundToInt(t.controlHeight + t.spacing));
        for (auto& row : rows)
        {
            auto line = r.removeFromTop(juce::roundToInt(t.controlHeight * 2.0f));
            line.removeFromTop(juce::roundToInt(t.fontSmall + 2.0f));
            row.remove->setBounds(line.removeFromRight(line.getHeight()));
            const int w = line.getWidth() / 2;
            row.boxes[0]->setBounds(line.removeFromLeft(w).reduced(1, 0));
            row.boxes[1]->setBounds(line.reduced(1, 0));
            row.labelArea = line.withY(line.getY() - juce::roundToInt(t.fontSmall + 2.0f)).withHeight(juce::roundToInt(t.fontSmall + 2.0f)).withX(0).withWidth(getWidth());
        }
    }

    void paint(juce::Graphics& g) override
    {
        if (!isActive())
            return;
        const auto& t = themeOf(*this);
        g.setFont(juce::Font(t.font(t.fontSmall)).boldened());
        g.setColour(t.accent);
        g.drawText(racks::macroName(rack, shownMacro).toUpperCase() + " MAPS", getLocalBounds().removeFromTop(juce::roundToInt(t.controlHeight)),
                   juce::Justification::centredLeft, true);
        g.setFont(t.font(t.fontSmall));
        if (rows.empty())
        {
            g.setColour(t.textDim);
            g.drawFittedText("Click a parameter of a device in this rack to map it.", getLocalBounds().withTrimmedTop(juce::roundToInt(t.controlHeight * 1.5f)),
                             juce::Justification::centredTop, 4);
        }
        for (const auto& row : rows)
        {
            g.setColour(t.text);
            g.drawText(row.label, row.labelArea, juce::Justification::centredLeft, true);
        }
    }

private:
    struct Row
    {
        juce::ValueTree mapping;
        juce::String label;
        std::vector<std::unique_ptr<NumberBox>> boxes;
        std::unique_ptr<IconButton> remove;
        juce::Rectangle<int> labelArea;
    };

    DeviceContext& context;
    juce::ValueTree rack;
    std::vector<Row> rows;
    int shownMacro = -1;
};

// --- Chains -----------------------------------------------------------------------------------------------

/** One chain in the list: its name (click selects it, double-click renames), mute, solo, volume
    and pan; for a pad also its note, choke group and tune. */
class ChainRowView final : public juce::Component
{
public:
    ChainRowView(DeviceContext& c, juce::ValueTree r, juce::ValueTree chainNode, bool isPad)
        : context(c), rack(std::move(r)), chain(std::move(chainNode)), pad(isPad)
    {
        setTitle(chain[ids::name].toString());
        mute = std::make_unique<Toggle>(chainValue(context, chain, ParameterSpec::toggle("chain.mute", "Mute", false).inGroup("Chain"), ids::mute),
                                        Toggle::Role::mute, "M");
        solo = std::make_unique<Toggle>(chainValue(context, chain, ParameterSpec::toggle("chain.solo", "Solo", false).inGroup("Chain"), ids::solo),
                                        Toggle::Role::solo, "S");
        volume = std::make_unique<NumberBox>(chainValue(context, chain, chainVolumeSpec(), ids::volumeDb));
        pan = std::make_unique<NumberBox>(chainValue(context, chain, chainPanSpec(), ids::pan));
        for (juce::Component* comp : {static_cast<juce::Component*>(mute.get()), static_cast<juce::Component*>(solo.get()),
                                      static_cast<juce::Component*>(volume.get()), static_cast<juce::Component*>(pan.get())})
            addAndMakeVisible(*comp);
        if (pad)
        {
            choke = std::make_unique<NumberBox>(chainValue(context, chain,
                                                           ParameterSpec::integer("pad.choke", "Choke", 0, 16, 0).inGroup("Pad").withFormatter([](float v) {
                                                               return v < 0.5f ? juce::String("-") : "C" + juce::String(juce::roundToInt(v));
                                                           }),
                                                           ids::chokeGroup));
            tune = std::make_unique<NumberBox>(chainValue(context, chain,
                                                          ParameterSpec::integer("pad.tune", "Tune", 0, 127, 60).inGroup("Pad").withFormatter([](float v) {
                                                              const int st = juce::roundToInt(v) - 60;
                                                              return st == 0 ? juce::String("0 st") : (st > 0 ? "+" : "") + juce::String(st) + " st";
                                                          }),
                                                          ids::playNote));
            choke->setTooltip("The pad's choke group: a pad cuts the others in its group (- for none).");
            tune->setTooltip("The pad's tuning, in semitones (the note its chain plays, from 60).");
            addAndMakeVisible(*choke);
            addAndMakeVisible(*tune);
        }
    }

    const juce::ValueTree& chainNode() const noexcept { return chain; }

    void resized() override
    {
        const auto& t = themeOf(*this);
        auto r = getLocalBounds().reduced(0, 1);
        const int unit = juce::roundToInt(t.controlHeight);
        pan->setBounds(r.removeFromRight(unit * 2).reduced(1, 0));
        volume->setBounds(r.removeFromRight(unit * 2 + unit / 2).reduced(1, 0));
        solo->setBounds(r.removeFromRight(unit).reduced(1, 0));
        mute->setBounds(r.removeFromRight(unit).reduced(1, 0));
        if (pad)
        {
            tune->setBounds(r.removeFromRight(unit * 2).reduced(1, 0));
            choke->setBounds(r.removeFromRight(unit + unit / 2).reduced(1, 0));
        }
        nameArea = r;
    }

    void paint(juce::Graphics& g) override
    {
        const auto& t = themeOf(*this);
        const bool selected = context.view != nullptr && context.view->selectedChainOf(rack) == chain;
        g.setColour(selected ? t.surfaceRaised.brighter(0.1f) : t.surfaceRaised);
        g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(0.0f, 1.0f), t.radius);
        if (selected)
        {
            g.setColour(t.selection);
            g.fillRect(getLocalBounds().removeFromLeft(juce::jmax(2, juce::roundToInt(t.stroke * 3.0f))).reduced(0, 2));
        }
        g.setFont(t.font(t.fontNormal));
        g.setColour(t.text);
        auto r = nameArea.reduced(juce::roundToInt(t.padding * 0.75f), 0);
        if (pad)
        {
            g.setColour(t.textDim);
            g.drawText(juce::String(static_cast<int>(chain.getProperty(ids::receiveNote, 0))), r.removeFromLeft(juce::roundToInt(t.controlHeight * 1.2f)),
                       juce::Justification::centredLeft, false);
            g.setColour(t.text);
        }
        g.drawText(chain[ids::name].toString(), r, juce::Justification::centredLeft, true);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        if (context.view != nullptr)
            context.view->selectChain(rack, chain);
        if (e.mods.isPopupMenu())
            menu().showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
    }
    void mouseDoubleClick(const juce::MouseEvent&) override { rename(); }

    void rename()
    {
        juce::Component::SafePointer<ChainRowView> safe(this);
        askForText(*this, pad ? "Rename Pad" : "Rename Chain", chain[ids::name].toString(), [safe](const juce::String& name) {
            if (safe == nullptr)
                return;
            safe->context.undo().beginNewTransaction("Rename chain");
            safe->chain.setProperty(ids::name, name, safe->context.set().undoManager());
        });
    }

    juce::PopupMenu menu()
    {
        juce::PopupMenu m;
        juce::Component::SafePointer<ChainRowView> safe(this);
        m.addItem("Rename...", [safe] {
            if (safe != nullptr)
                safe->rename();
        });
        m.addItem("Duplicate", [safe] {
            if (safe == nullptr)
                return;
            auto& c = safe->context;
            c.undo().beginNewTransaction("Duplicate chain");
            auto copy = racks::addChain(c.set(), safe->rack, safe->chain[ids::name].toString() + " Copy");
            for (const auto& d : racks::devicesOf(safe->chain))
                c.set().insertDeviceCopy(racks::devicesOf(copy), -1, d);
            for (const auto& id : {ids::volumeDb, ids::pan, ids::keyLow, ids::keyHigh, ids::keyFadeLow, ids::keyFadeHigh, ids::velocityLow,
                                   ids::velocityHigh, ids::velocityFadeLow, ids::velocityFadeHigh})
                if (safe->chain.hasProperty(id))
                    copy.setProperty(id, safe->chain[id], c.set().undoManager());
            if (c.view != nullptr)
                c.view->selectChain(safe->rack, copy);
        });
        m.addSeparator();
        m.addItem(pad ? "Remove Pad" : "Delete Chain", [safe] {
            if (safe == nullptr)
                return;
            safe->context.undo().beginNewTransaction(safe->pad ? "Remove pad" : "Delete chain");
            racks::removeChain(safe->context.set(), safe->chain);
        });
        return m;
    }

private:
    DeviceContext& context;
    juce::ValueTree rack, chain;
    bool pad;
    std::unique_ptr<Toggle> mute, solo;
    std::unique_ptr<NumberBox> volume, pan, choke, tune;
    juce::Rectangle<int> nameArea;
};

class ChainList final : public juce::Component
{
public:
    ChainList(DeviceContext& c, juce::ValueTree r, bool drum) : context(c), rack(std::move(r)), pads(drum)
    {
        setTitle(pads ? "Pads" : "Chains");
        add.onClick = [this] {
            context.undo().beginNewTransaction("Add chain");
            const auto chain = racks::addChain(context.set(), rack);
            if (context.view != nullptr)
                context.view->selectChain(rack, chain);
        };
        addChildComponent(add);
        add.setVisible(!pads);
        zones.setClickingTogglesState(true);
        zones.setWantsKeyboardFocus(false);
        zones.setTooltip("Shows each chain's key and velocity zones.");
        zones.onClick = [this] {
            if (onZonesToggled)
                onZonesToggled(zones.getToggleState());
        };
        addChildComponent(zones);
        zones.setVisible(RackDevice::kindForType(rack[ids::deviceType].toString()) == RackKind::Instrument);
        rowsView.setScrollBarsShown(true, false);
        rowsView.setViewedComponent(&rowsHolder, false);
        addAndMakeVisible(rowsView);
        rebuild();
    }

    std::function<void(bool)> onZonesToggled;
    bool zonesShown() const { return zones.getToggleState(); }

    void rebuild()
    {
        rows.clear();
        for (const auto& chain : rack.getChildWithName(ids::rackChains))
        {
            auto row = std::make_unique<ChainRowView>(context, rack, chain, pads);
            rowsHolder.addAndMakeVisible(*row);
            rows.push_back(std::move(row));
        }
        if (pads) // in note order
            std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
                return static_cast<int>(a->chainNode().getProperty(ids::receiveNote, 0)) < static_cast<int>(b->chainNode().getProperty(ids::receiveNote, 0));
            });
        resized();
    }
    void refresh()
    {
        for (auto& r : rows)
            r->repaint();
    }
    const std::vector<std::unique_ptr<ChainRowView>>& chainRows() const noexcept { return rows; }

    int preferredWidth(int) const { return juce::roundToInt(themeOf(*this).controlHeight * (pads ? 14.5f : 12.0f)); }

    void resized() override
    {
        const auto& t = themeOf(*this);
        auto r = getLocalBounds();
        auto header = r.removeFromTop(juce::roundToInt(t.controlHeight));
        r.removeFromTop(juce::roundToInt(t.spacing));
        add.setBounds(header.removeFromRight(header.getHeight()));
        zones.setBounds(header.removeFromRight(juce::roundToInt(t.controlHeight * 2.6f)).reduced(1, 0));
        rowsView.setBounds(r);
        const int h = juce::roundToInt(t.controlHeight * 1.15f);
        rowsHolder.setSize(r.getWidth() - (static_cast<int>(rows.size()) * h > r.getHeight() ? rowsView.getScrollBarThickness() : 0),
                           juce::jmax(r.getHeight(), static_cast<int>(rows.size()) * h));
        for (int i = 0; i < static_cast<int>(rows.size()); ++i)
            rows[static_cast<std::size_t>(i)]->setBounds(0, i * h, rowsHolder.getWidth(), h);
    }

    void paint(juce::Graphics& g) override
    {
        const auto& t = themeOf(*this);
        g.setFont(juce::Font(t.font(t.fontSmall)).boldened());
        g.setColour(t.textDim);
        g.drawText(pads ? "PADS" : "CHAINS", getLocalBounds().removeFromTop(juce::roundToInt(t.controlHeight)), juce::Justification::centredLeft, false);
        if (rows.empty())
        {
            g.setFont(t.font(t.fontSmall));
            g.drawFittedText(pads ? "Drop samples on the pads." : "No chains: click + to add one.",
                             getLocalBounds().withTrimmedTop(juce::roundToInt(t.controlHeight * 1.5f)), juce::Justification::centredTop, 3);
        }
    }

private:
    DeviceContext& context;
    juce::ValueTree rack;
    bool pads;
    IconButton add{"Add Chain", Icon::plus, "Adds a chain to the rack."};
    juce::TextButton zones{"Zones"};
    juce::Viewport rowsView;
    juce::Component rowsHolder;
    std::vector<std::unique_ptr<ChainRowView>> rows;
};

// --- Zones -------------------------------------------------------------------------------------------------

/** Each chain's key zone (or velocity zone) as a bar across 0-127, with its fades drawn as slopes.
    Drag an end to move it, the handle at the top of a slope to set the fade, the middle to move the
    whole zone. */
class ZoneEditor final : public juce::Component
{
public:
    ZoneEditor(DeviceContext& c, juce::ValueTree r) : context(c), rack(std::move(r))
    {
        setTitle("Zones");
        setDescription("Drag a zone's ends to set its range, the handles at the top to set its fades, its middle to move it.");
        mode.setClickingTogglesState(true);
        mode.setWantsKeyboardFocus(false);
        mode.setTooltip("Shows the velocity zones instead of the key zones.");
        mode.onClick = [this] {
            mode.setButtonText(velocity() ? "Velocity" : "Key");
            repaint();
        };
        addAndMakeVisible(mode);
    }

    bool velocity() const { return mode.getToggleState(); }
    void setVelocity(bool v)
    {
        mode.setToggleState(v, juce::dontSendNotification);
        mode.setButtonText(v ? "Velocity" : "Key");
        repaint();
    }
    int minimum() const { return velocity() ? 1 : 0; }

    rackmath::Zone zoneOf(const juce::ValueTree& chain) const
    {
        if (velocity())
            return {chain.getProperty(ids::velocityLow, 1), chain.getProperty(ids::velocityHigh, 127), chain.getProperty(ids::velocityFadeLow, 0),
                    chain.getProperty(ids::velocityFadeHigh, 0)};
        return {chain.getProperty(ids::keyLow, 0), chain.getProperty(ids::keyHigh, 127), chain.getProperty(ids::keyFadeLow, 0),
                chain.getProperty(ids::keyFadeHigh, 0)};
    }
    void setZone(const juce::ValueTree& chain, const rackmath::Zone& z)
    {
        if (velocity())
            racks::setVelocityZone(context.set(), chain, z.low, z.high, z.fadeLow, z.fadeHigh);
        else
            racks::setKeyZone(context.set(), chain, z.low, z.high, z.fadeLow, z.fadeHigh);
    }

    juce::Rectangle<float> barsArea() const
    {
        const auto& t = themeOf(*this);
        return getLocalBounds().toFloat().withTrimmedTop(t.controlHeight + t.spacing).reduced(t.padding * 0.5f, 0.0f);
    }
    juce::Rectangle<float> barFor(int index) const
    {
        const auto area = barsArea();
        const int n = juce::jmax(1, rack.getChildWithName(ids::rackChains).getNumChildren());
        const float h = juce::jmin(themeOf(*this).controlHeight * 1.15f, area.getHeight() / static_cast<float>(n));
        return {area.getX(), area.getY() + h * static_cast<float>(index), area.getWidth(), h};
    }

    int preferredWidth(int) const { return juce::roundToInt(themeOf(*this).controlHeight * 12.0f); }
    void resized() override
    {
        const auto& t = themeOf(*this);
        mode.setBounds(getLocalBounds().removeFromTop(juce::roundToInt(t.controlHeight)).removeFromRight(juce::roundToInt(t.controlHeight * 3.5f)));
    }

    void paint(juce::Graphics& g) override
    {
        const auto& t = themeOf(*this);
        g.setFont(juce::Font(t.font(t.fontSmall)).boldened());
        g.setColour(t.textDim);
        g.drawText(velocity() ? "VELOCITY ZONES" : "KEY ZONES", getLocalBounds().removeFromTop(juce::roundToInt(t.controlHeight)),
                   juce::Justification::centredLeft, false);
        const auto area = barsArea();
        g.setColour(t.surfaceSunken);
        g.fillRect(area);
        // Octave lines (keys) or quarters (velocity).
        g.setColour(t.border.withAlpha(0.6f));
        for (int v = velocity() ? 32 : 12; v < 128; v += velocity() ? 32 : 12)
            g.drawVerticalLine(juce::roundToInt(rackmath::zoneX(v, area.getX(), area.getWidth(), minimum())), area.getY(), area.getBottom());
        int i = 0;
        for (const auto& chain : rack.getChildWithName(ids::rackChains))
        {
            const auto bar = barFor(i++).reduced(0.0f, 2.0f);
            const auto z = zoneOf(chain);
            const bool selected = context.view != nullptr && context.view->selectedChainOf(rack) == chain;
            const float lx = rackmath::zoneX(z.low, bar.getX(), bar.getWidth(), minimum());
            const float fl = rackmath::zoneX(z.low + z.fadeLow, bar.getX(), bar.getWidth(), minimum());
            const float fh = rackmath::zoneX(z.high + 1 - z.fadeHigh, bar.getX(), bar.getWidth(), minimum());
            const float hx = rackmath::zoneX(z.high + 1, bar.getX(), bar.getWidth(), minimum());
            juce::Path p;
            p.startNewSubPath(lx, bar.getBottom());
            p.lineTo(fl, bar.getY());
            p.lineTo(fh, bar.getY());
            p.lineTo(hx, bar.getBottom());
            p.closeSubPath();
            g.setColour((selected ? t.selection : t.accent).withAlpha(0.45f));
            g.fillPath(p);
            g.setColour(selected ? t.selection : t.accent);
            g.strokePath(p, juce::PathStrokeType(t.stroke));
            const float s = juce::jmax(4.0f, t.controlHeight * 0.25f);
            for (const float x : {fl, fh})
                g.fillRect(juce::Rectangle<float>(s, s).withCentre({x, bar.getY() + s * 0.5f}));
            g.setColour(t.text);
            g.setFont(t.font(t.fontSmall));
            g.drawText(chain[ids::name].toString() + "  " + juce::String(z.low) + "-" + juce::String(z.high), bar.reduced(t.padding * 0.5f, 0.0f),
                       juce::Justification::centredLeft, true);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        dragging = {};
        handle = rackmath::ZoneHandle::none;
        int i = 0;
        for (const auto& chain : rack.getChildWithName(ids::rackChains))
        {
            const auto bar = barFor(i++);
            const auto h = rackmath::zoneHandleAt(zoneOf(chain), e.position.x, e.position.y, bar.getX(), bar.getWidth(), bar.getY(), bar.getHeight(),
                                                  themeOf(*this).controlHeight * 0.3f, minimum());
            if (h != rackmath::ZoneHandle::none)
            {
                dragging = chain;
                handle = h;
                start = zoneOf(chain);
                startValue = rackmath::zoneValueAt(e.position.x, bar.getX(), bar.getWidth(), minimum());
                context.undo().beginNewTransaction(velocity() ? "Velocity zone" : "Key zone");
                if (context.view != nullptr)
                    context.view->selectChain(rack, chain);
                return;
            }
        }
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (!dragging.isValid())
            return;
        const auto area = barsArea();
        const int delta = rackmath::zoneValueAt(e.position.x, area.getX(), area.getWidth(), minimum()) - startValue;
        const auto z = rackmath::dragZone(start, handle, delta, minimum());
        if (!(z == zoneOf(dragging)))
            setZone(dragging, z);
    }
    void mouseUp(const juce::MouseEvent&) override { dragging = {}; }

private:
    DeviceContext& context;
    juce::ValueTree rack;
    juce::TextButton mode{"Key"};
    juce::ValueTree dragging;
    rackmath::ZoneHandle handle = rackmath::ZoneHandle::none;
    rackmath::Zone start;
    int startValue = 0;
};

// --- Drum pads ---------------------------------------------------------------------------------------------

class PadGrid final : public juce::Component, public juce::DragAndDropTarget, public juce::FileDragAndDropTarget
{
public:
    PadGrid(DeviceContext& c, juce::ValueTree r) : context(c), rack(std::move(r))
    {
        setTitle("Pads");
        setDescription("Click a pad to play it and show its chain; drag it onto another to move or swap it; drop samples on pads.");
        for (auto* b : std::initializer_list<juce::Component*>{&up, &down, &pageSize})
            addAndMakeVisible(*b);
        up.onClick = [this] { scrollRows(1); };
        down.onClick = [this] { scrollRows(-1); };
        pageSize.onClick = [this] { pageMenu().showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&pageSize)); };
    }

    rackmath::PadPage page() const
    {
        const auto area = padArea();
        return rackmath::fitPads(area.getWidth(), area.getHeight(), minPad(), rack.getProperty(ids::padPage, 0));
    }
    int firstNote() const
    {
        if (rack.hasProperty(ids::padStart))
            return juce::jlimit(0, 127, static_cast<int>(rack[ids::padStart]));
        // As the computer keyboard finds it (the page with the lowest pad).
        int lowest = 128;
        for (const auto& pad : rack.getChildWithName(ids::rackChains))
            lowest = juce::jmin(lowest, static_cast<int>(pad.getProperty(ids::receiveNote, 128)));
        const int note = lowest < 128 ? lowest : 36;
        return juce::jlimit(0, 112, 36 + 16 * static_cast<int>(std::floor((note - 36) / 16.0)));
    }
    void scrollRows(int rows)
    {
        const auto p = page();
        const int start = juce::jlimit(0, juce::jmax(0, 128 - p.size()), firstNote() + rows * p.columns);
        rack.setProperty(ids::padStart, start, nullptr); // the view's state, not an edit
        repaint();
    }

    int minPad() const { return juce::roundToInt(themeOf(*this).controlHeight * 2.0f); }
    int preferredWidth(int height) const
    {
        const auto& t = themeOf(*this);
        const int gridHeight = height - juce::roundToInt(t.controlHeight + t.spacing);
        const auto p = rackmath::fitPads(10000, gridHeight, minPad(), rack.getProperty(ids::padPage, 0));
        const int pad = juce::jmax(minPad(), gridHeight / p.rows);
        return p.columns * pad;
    }

    juce::Rectangle<int> padArea() const
    {
        const auto& t = themeOf(*this);
        return getLocalBounds().withTrimmedTop(juce::roundToInt(t.controlHeight + t.spacing));
    }
    /** Pad i of the page: 0 is bottom left, as on Push and in Live. */
    juce::Rectangle<int> padBounds(int i) const
    {
        const auto area = padArea();
        const auto p = page();
        const int w = area.getWidth() / p.columns, h = area.getHeight() / p.rows;
        return {area.getX() + (i % p.columns) * w, area.getBottom() - (i / p.columns + 1) * h, w, h};
    }
    int padAt(juce::Point<int> pt) const
    {
        for (int i = 0; i < page().size(); ++i)
            if (padBounds(i).contains(pt))
                return i;
        return -1;
    }
    int noteAt(juce::Point<int> pt) const
    {
        const int i = padAt(pt);
        return i >= 0 && firstNote() + i <= 127 ? firstNote() + i : -1;
    }

    void resized() override
    {
        const auto& t = themeOf(*this);
        auto bar = getLocalBounds().removeFromTop(juce::roundToInt(t.controlHeight));
        up.setBounds(bar.removeFromRight(bar.getHeight()));
        down.setBounds(bar.removeFromRight(bar.getHeight()));
        pageSize.setBounds(bar.removeFromRight(juce::roundToInt(t.controlHeight * 2.0f)).reduced(1, 0));
    }

    void paint(juce::Graphics& g) override
    {
        const auto& t = themeOf(*this);
        const auto p = page();
        g.setFont(juce::Font(t.font(t.fontSmall)).boldened());
        g.setColour(t.textDim);
        g.drawText("PADS " + juce::String(firstNote()) + "-" + juce::String(juce::jmin(127, firstNote() + p.size() - 1)),
                   getLocalBounds().removeFromTop(juce::roundToInt(t.controlHeight)), juce::Justification::centredLeft, false);
        const auto selected = context.view != nullptr ? context.view->selectedChainOf(rack) : juce::ValueTree();
        bool anySolo = false;
        for (const auto& pad : rack.getChildWithName(ids::rackChains))
            anySolo = anySolo || static_cast<bool>(pad[ids::solo]);
        for (int i = 0; i < p.size(); ++i)
        {
            const int note = firstNote() + i;
            if (note > 127)
                break;
            const auto r = padBounds(i).toFloat().reduced(1.5f);
            const auto pad = racks::padFor(rack, note);
            auto fill = pad.isValid() ? t.surfaceRaised : t.surfaceSunken;
            if (note == held)
                fill = t.play.withAlpha(0.6f);
            else if (pad.isValid() && pad == selected)
                fill = t.selection.withAlpha(0.45f);
            if (i == hover)
                fill = fill.brighter(0.15f);
            g.setColour(fill);
            g.fillRoundedRectangle(r, t.radius);
            g.setColour(pad.isValid() && pad == selected ? t.selection : t.border);
            g.drawRoundedRectangle(r, t.radius, t.stroke);
            auto text = r.reduced(t.padding * 0.5f, 2.0f);
            g.setFont(t.font(t.fontSmall));
            g.setColour(t.textDim);
            g.drawText(juce::MidiMessage::getMidiNoteName(note, true, true, 4), text, juce::Justification::topLeft, false);
            if (!pad.isValid())
                continue;
            const bool silent = static_cast<bool>(pad[ids::mute]) || (anySolo && !static_cast<bool>(pad[ids::solo]));
            juce::String marks;
            if (const int choke = pad.getProperty(ids::chokeGroup, 0); choke > 0)
                marks << "C" << choke << " ";
            if (static_cast<bool>(pad[ids::solo]))
                marks << "S";
            if (static_cast<bool>(pad[ids::mute]))
                marks << "M";
            g.drawText(marks.trim(), text, juce::Justification::topRight, false);
            g.setColour(silent ? t.textDisabled : t.text);
            g.drawFittedText(pad[ids::name].toString(), text.withTrimmedTop(t.fontSmall + 2.0f).toNearestInt(), juce::Justification::centred, 2, 0.8f);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const int note = noteAt(e.getPosition());
        if (note < 0)
            return;
        const auto pad = racks::padFor(rack, note);
        if (e.mods.isPopupMenu())
        {
            padMenu(note).showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
            return;
        }
        if (pad.isValid() && context.view != nullptr)
            context.view->selectChain(rack, pad);
        // Play it, whatever the track's arm (as the piano roll auditions notes).
        if (const int track = trackIndexOf(context, rack); track >= 0)
        {
            const int i = padAt(e.getPosition());
            const auto r = padBounds(i);
            const int velocity = juce::jlimit(1, 127, juce::roundToInt(127.0f - 96.0f * static_cast<float>(e.y - r.getY()) / juce::jmax(1, r.getHeight())));
            context.session.previewNote(track, note, velocity);
            held = note;
        }
        repaint();
    }
    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (e.getDistanceFromDragStart() < 6)
            return;
        stopPlaying();
        const int note = noteAt(e.getMouseDownPosition());
        if (note < 0 || !racks::padFor(rack, note).isValid())
            return;
        if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this); container != nullptr && !container->isDragAndDropActive())
            container->startDragging(drag::pad(note), this, juce::ScaledImage(createComponentSnapshot(padBounds(padAt(e.getMouseDownPosition())))));
    }
    void mouseUp(const juce::MouseEvent&) override { stopPlaying(); }

    /** A sample on a note: a new pad, or a new sample in the pad's Sampler. */
    void assign(int note, const juce::File& file, bool newTransaction)
    {
        if (newTransaction)
            context.undo().beginNewTransaction("Load sample");
        auto pad = racks::padFor(rack, note);
        if (!pad.isValid())
        {
            pad = racks::addDrumPad(context.set(), rack, note, file.getFullPathName());
            pad.setProperty(ids::name, file.getFileNameWithoutExtension(), context.set().undoManager());
        }
        else
        {
            for (const auto& d : racks::devicesOf(pad))
                if (d[ids::deviceType].toString() == "lattice.sampler")
                {
                    loadSampleInto(context, d, file, false);
                    break;
                }
            pad.setProperty(ids::name, file.getFileNameWithoutExtension(), context.set().undoManager());
        }
        if (context.view != nullptr)
            context.view->selectChain(rack, pad);
    }
    /** Moves the pad on one note to another (swapping with a pad there), as one step. */
    void movePad(int from, int to)
    {
        if (from == to || from < 0 || to < 0)
            return;
        context.undo().beginNewTransaction("Move pad");
        racks::movePad(context.set(), rack, from, to);
    }

    // Drops: samples (several fill pads from the one they land on) and pads.
    bool isInterestedInDragSource(const SourceDetails& d) override { return drag::is(d.description, "sample") || drag::is(d.description, "pad"); }
    void itemDragMove(const SourceDetails& d) override { setHover(padAt(d.localPosition)); }
    void itemDragExit(const SourceDetails&) override { setHover(-1); }
    void itemDropped(const SourceDetails& d) override
    {
        const int note = noteAt(d.localPosition);
        setHover(-1);
        if (note < 0)
            return;
        if (drag::is(d.description, "pad"))
            movePad(drag::payload(d.description).getIntValue(), note);
        else if (isSampleFile(juce::File(drag::payload(d.description))))
            assign(note, juce::File(drag::payload(d.description)), true);
    }
    bool isInterestedInFileDrag(const juce::StringArray& files) override
    {
        return std::any_of(files.begin(), files.end(), [](const juce::String& f) { return isSampleFile(juce::File(f)); });
    }
    void fileDragMove(const juce::StringArray&, int x, int y) override { setHover(padAt({x, y})); }
    void fileDragExit(const juce::StringArray&) override { setHover(-1); }
    void filesDropped(const juce::StringArray& files, int x, int y) override
    {
        int note = noteAt({x, y});
        setHover(-1);
        if (note < 0)
            return;
        context.undo().beginNewTransaction("Load samples");
        for (const auto& f : files)
            if (isSampleFile(juce::File(f)) && note <= 127)
                assign(note++, juce::File(f), false);
    }

private:
    void stopPlaying()
    {
        if (held >= 0)
            if (const int track = trackIndexOf(context, rack); track >= 0)
                context.session.previewNote(track, held, 0);
        held = -1;
        repaint();
    }
    void setHover(int i)
    {
        if (i != hover)
        {
            hover = i;
            repaint();
        }
    }

    juce::PopupMenu pageMenu()
    {
        juce::PopupMenu m;
        const int current = rack.getProperty(ids::padPage, 0);
        for (const auto& [size, name] : {std::pair{0, "Fit the Space"}, std::pair{16, "16 Pads (4 x 4)"}, std::pair{32, "32 Pads (4 x 8)"},
                                         std::pair{64, "64 Pads (8 x 8)"}})
            m.addItem(name, true, current == size, [this, size = size] {
                rack.setProperty(ids::padPage, size, nullptr);
                if (context.view != nullptr)
                    context.view->requestLayout();
                repaint();
            });
        return m;
    }

    juce::PopupMenu padMenu(int note)
    {
        juce::PopupMenu m;
        const auto pad = racks::padFor(rack, note);
        m.addSectionHeader("Pad " + juce::MidiMessage::getMidiNoteName(note, true, true, 4) + " (" + juce::String(note) + ")");
        juce::Component::SafePointer<PadGrid> safe(this);
        m.addItem("Load Sample...", [safe, note] {
            if (safe == nullptr || !safe->context.chooseFile)
                return;
            safe->context.chooseFile("Load a sample for the pad", sampleFilePatterns, [safe, note](const juce::File& f) {
                if (safe != nullptr)
                    safe->assign(note, f, true);
            });
        });
        if (!pad.isValid())
            m.addItem("Add Empty Pad", [safe, note] {
                if (safe == nullptr)
                    return;
                safe->context.undo().beginNewTransaction("Add pad");
                const auto added = racks::addDrumPad(safe->context.set(), safe->rack, note);
                if (safe->context.view != nullptr)
                    safe->context.view->selectChain(safe->rack, added);
            });
        else
        {
            juce::PopupMenu choke;
            const int current = pad.getProperty(ids::chokeGroup, 0);
            for (int group = 0; group <= 16; ++group)
                choke.addItem(group == 0 ? juce::String("None") : juce::String(group), true, group == current, [safe, pad, group] {
                    if (safe == nullptr)
                        return;
                    safe->context.undo().beginNewTransaction("Choke group");
                    racks::setChokeGroup(safe->context.set(), pad, group);
                });
            m.addSubMenu("Choke Group", choke);
            m.addItem("Remove Pad", [safe, pad] {
                if (safe == nullptr)
                    return;
                safe->context.undo().beginNewTransaction("Remove pad");
                racks::removeChain(safe->context.set(), pad);
            });
        }
        m.addSeparator();
        const bool tuned = rack.getChildWithName(ids::deviceState).getProperty(tunedPadsId, false);
        m.addItem("Tuned Pads (follow the track's tuning)", true, tuned, [safe, tuned] {
            if (safe == nullptr)
                return;
            safe->context.undo().beginNewTransaction("Tuned pads");
            racks::setTunedPads(safe->context.set(), safe->rack, !tuned);
        });
        return m;
    }

    DeviceContext& context;
    juce::ValueTree rack;
    IconButton up{"Pads Up", Icon::up, "Shows the next row of pads up."};
    IconButton down{"Pads Down", Icon::foldOpen, "Shows the next row of pads down."};
    juce::TextButton pageSize{"Page"};
    int held = -1;
    int hover = -1;
};

// --- The panel ------------------------------------------------------------------------------------------------

class RackPanel final : public DeviceBody, private juce::ValueTree::Listener, private juce::AsyncUpdater
{
public:
    RackPanel(DeviceContext& c, const juce::ValueTree& d) : DeviceBody(c, d)
    {
        drum = RackDevice::kindForType(node[ids::deviceType].toString()) == RackKind::Drum;
        macros = std::make_unique<MacroGrid>(context, node);
        addAndMakeVisible(*macros);
        mappings = std::make_unique<MappingList>(context, node);
        addAndMakeVisible(*mappings);
        if (drum)
        {
            pads = std::make_unique<PadGrid>(context, node);
            addAndMakeVisible(*pads);
        }
        chains = std::make_unique<ChainList>(context, node, drum);
        chains->onZonesToggled = [this](bool on) {
            zones->setVisible(on);
            resized();
            widthChanged();
        };
        addAndMakeVisible(*chains);
        zones = std::make_unique<ZoneEditor>(context, node);
        addChildComponent(*zones);
        showSelectedChain();
        mappings->rebuild();
        node.addListener(this);
    }
    ~RackPanel() override
    {
        cancelPendingUpdate();
        node.removeListener(this);
    }

    MacroGrid& macroGrid() { return *macros; }
    ChainList& chainList() { return *chains; }
    ZoneEditor& zoneEditor() { return *zones; }
    PadGrid* padGrid() { return pads.get(); }
    MappingList& mappingList() { return *mappings; }
    ChainView* inner() { return innerChain.get(); }

    int preferredWidth(int height) const override
    {
        const auto& t = themeOf(*this);
        const int gap = juce::roundToInt(t.padding);
        int w = macros->preferredWidth(height) + gap;
        if (mappings->isActive())
            w += mappings->preferredWidth(height) + gap;
        if (pads != nullptr)
            w += pads->preferredWidth(height) + gap;
        w += chains->preferredWidth(height) + gap;
        if (zones->isVisible())
            w += zones->preferredWidth(height) + gap;
        if (innerChain != nullptr)
            w += innerChain->preferredWidth(height);
        return w;
    }

    void resized() override
    {
        const auto& t = themeOf(*this);
        const int gap = juce::roundToInt(t.padding);
        auto r = getLocalBounds();
        macros->setBounds(r.removeFromLeft(macros->preferredWidth(r.getHeight())));
        r.removeFromLeft(gap);
        mappings->setVisible(mappings->isActive());
        if (mappings->isActive())
        {
            mappings->setBounds(r.removeFromLeft(mappings->preferredWidth(r.getHeight())));
            r.removeFromLeft(gap);
        }
        if (pads != nullptr)
        {
            pads->setBounds(r.removeFromLeft(pads->preferredWidth(r.getHeight())));
            r.removeFromLeft(gap);
        }
        chains->setBounds(r.removeFromLeft(chains->preferredWidth(r.getHeight())));
        r.removeFromLeft(gap);
        if (zones->isVisible())
        {
            zones->setBounds(r.removeFromLeft(zones->preferredWidth(r.getHeight())));
            r.removeFromLeft(gap);
        }
        innerArea = r;
        if (innerChain != nullptr)
            innerChain->setBounds(r.withWidth(juce::jmax(r.getWidth(), innerChain->preferredWidth(r.getHeight()))));
    }

    void paint(juce::Graphics& g) override
    {
        if (innerChain == nullptr)
            return;
        // The selected chain's devices sit in a well, joined to the chain list.
        const auto& t = themeOf(*this);
        g.setColour(t.surfaceSunken.withAlpha(0.6f));
        g.fillRoundedRectangle(innerChain->getBounds().toFloat().expanded(2.0f), t.radius);
    }

    void flushUpdates() override
    {
        handleUpdateNowIfNeeded();
        if (innerChain != nullptr)
            innerChain->flushUpdates();
    }

    void viewStateChanged() override
    {
        showSelectedChain();
        macros->mapModeChanged();
        mappings->rebuild();
        chains->refresh();
        zones->repaint();
        if (pads != nullptr)
            pads->repaint();
        resized();
        widthChanged();
    }

    bool reveal(const juce::String& id) override { return controlFor(id) != nullptr; }

private:
    void showSelectedChain()
    {
        const auto chain = context.view != nullptr ? context.view->selectedChainOf(node) : node.getChildWithName(ids::rackChains).getChild(0);
        const auto devices = racks::devicesOf(chain);
        if (innerChain != nullptr && innerChain->chain() == devices)
            return;
        innerChain.reset();
        if (devices.isValid())
        {
            innerChain = std::make_unique<ChainView>(context, devices);
            addAndMakeVisible(*innerChain);
        }
        resized();
    }

    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override
    {
        if (tree.hasType(ids::deviceState) && tree.getParent() == node && property == macroCountId)
            pending |= rebuildMacros;
        else if (tree.hasType("MACRO") && tree.getParent().getParent() == node)
            pending |= renameMacros;
        else if (tree.hasType(ids::rackChain) && tree.getParent().getParent() == node)
            pending |= refreshChains | (property == ids::receiveNote ? rebuildChains : 0u);
        else if (tree.hasType(ids::mapping) && tree.getParent().getParent() == node)
            return; // a range: its boxes follow it
        else if (tree == node && (property == ids::padStart || property == ids::padPage))
            pending |= relayout;
        else
            return;
        triggerAsyncUpdate();
    }
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child) override { childrenChanged(parent, child); }
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int) override { childrenChanged(parent, child); }
    void valueTreeChildOrderChanged(juce::ValueTree& parent, int, int) override
    {
        if (parent.hasType(ids::rackChains) && parent.getParent() == node)
        {
            pending |= rebuildChains;
            triggerAsyncUpdate();
        }
    }
    void childrenChanged(juce::ValueTree& parent, juce::ValueTree& child)
    {
        if (parent.hasType(ids::rackChains) && parent.getParent() == node)
            pending |= rebuildChains;
        else if (parent.hasType(ids::mappings) && parent.getParent() == node)
            pending |= rebuildMappings;
        else if (parent.hasType(ids::deviceState) && parent.getParent() == node && child.hasType("MACRO"))
            pending |= renameMacros;
        else if (parent == node)
            pending |= rebuildChains | rebuildMappings | rebuildMacros;
        else
            return;
        triggerAsyncUpdate();
    }

    void handleAsyncUpdate() override
    {
        const auto f = std::exchange(pending, 0u);
        if ((f & rebuildMacros) != 0)
            macros->rebuild();
        else if ((f & renameMacros) != 0)
            macros->refreshNames();
        if ((f & rebuildChains) != 0)
        {
            chains->rebuild();
            showSelectedChain();
        }
        else if ((f & refreshChains) != 0)
            chains->refresh();
        if ((f & (refreshChains | rebuildChains)) != 0)
        {
            zones->repaint();
            if (pads != nullptr)
                pads->repaint();
        }
        if ((f & rebuildMappings) != 0)
            mappings->rebuild();
        resized();
        widthChanged();
    }

    enum : unsigned
    {
        rebuildMacros = 1,
        renameMacros = 2,
        rebuildChains = 4,
        refreshChains = 8,
        rebuildMappings = 16,
        relayout = 32
    };

    bool drum = false;
    std::unique_ptr<MacroGrid> macros;
    std::unique_ptr<MappingList> mappings;
    std::unique_ptr<PadGrid> pads;
    std::unique_ptr<ChainList> chains;
    std::unique_ptr<ZoneEditor> zones;
    std::unique_ptr<ChainView> innerChain;
    juce::Rectangle<int> innerArea;
    unsigned pending = 0;
};
} // namespace

std::unique_ptr<DeviceBody> rack(DeviceContext& c, const juce::ValueTree& d)
{
    return std::make_unique<RackPanel>(c, d);
}

} // namespace lattice::ui::panels
