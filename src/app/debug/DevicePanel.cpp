#include "app/debug/DevicePanel.h"

#include "devices/ChainEditing.h"
#include "devices/racks/Rack.h"
#include "devices/racks/RackEditing.h"

#include <algorithm>
#include <cmath>

namespace lattice::debugui
{

namespace
{
constexpr int sectionPad = 6;
constexpr int sectionTitle = 16;
constexpr int chainListWidth = 236;
constexpr int padGridWidth = 236;
constexpr int sampleWidth = 180;
constexpr int sidechainWidth = 170;
constexpr auto samplerType = "lattice.sampler";

/** Rows of controls that fit in a section of this height. */
int rowsFor(int bodyHeight)
{
    return std::max(1, (bodyHeight - sectionTitle - sectionPad) / ParameterControl::height);
}

/** Controls in columns of `rows`, each column as wide as its widest control. Lays them out from
    `origin` when given; returns the total width. */
int layOutColumns(const std::vector<ParameterControl*>& items, int rows, juce::Point<int>* origin)
{
    int x = 0;
    for (std::size_t first = 0; first < items.size(); first += static_cast<std::size_t>(rows))
    {
        const auto last = std::min(items.size(), first + static_cast<std::size_t>(rows));
        int w = 0;
        for (auto i = first; i < last; ++i)
            w = std::max(w, items[i]->preferredWidth());
        if (origin != nullptr)
            for (auto i = first; i < last; ++i)
                items[i]->setBounds(origin->x + x, origin->y + static_cast<int>(i - first) * ParameterControl::height, w,
                                    ParameterControl::height);
        x += w;
    }
    return x;
}

juce::String deviceName(const EditorContext& context, const juce::ValueTree& device)
{
    const auto type = device[ids::deviceType].toString();
    const auto* info = context.registry().find(type);
    return info != nullptr ? info->name : type + " (missing)";
}

void drawSectionFrame(juce::Graphics& g, juce::Rectangle<int> r, const juce::String& title)
{
    g.setColour(colours::background.brighter(0.06f));
    g.fillRoundedRectangle(r.toFloat().reduced(2.0f), 3.0f);
    g.setColour(colours::dimText);
    g.setFont(juce::FontOptions(11.0f, juce::Font::bold));
    g.drawText(title, r.removeFromTop(sectionTitle).reduced(6, 0), juce::Justification::centredLeft);
}

/** Arms a track for live input the way Live's selection arm does, so pads and keys reach it. */
void armOwnerTrack(EditorContext& context, const juce::ValueTree& device)
{
    const auto owner = LiveSet::chainOwnerOf(device);
    if (!owner.hasType(ids::track) || static_cast<bool>(owner[ids::arm]))
        return;
    const int index = context.set().trackIndexForId(LiveSet::idOf(owner));
    if (index >= 0)
    {
        context.undo().beginNewTransaction("Arm");
        context.set().setTrackArm(index, true, false);
    }
}
} // namespace

// --- Section: one parameter group ------------------------------------------------------------------

class DevicePanel::Section : public juce::Component
{
public:
    explicit Section(juce::String name) : title(std::move(name)) {}

    void add(std::unique_ptr<ParameterControl> control)
    {
        addAndMakeVisible(*control);
        pointers.push_back(control.get());
        items.push_back(std::move(control));
    }
    const std::vector<ParameterControl*>& controls() const noexcept { return pointers; }

    /** The width for a section this tall. */
    virtual int preferredWidth(int height) const
    {
        return std::max(40, layOutColumns(pointers, rowsFor(height), nullptr)) + 2 * sectionPad;
    }

    void paint(juce::Graphics& g) override { drawSectionFrame(g, getLocalBounds(), title); }

    void resized() override
    {
        auto origin = getLocalBounds().reduced(sectionPad, 0).withTrimmedTop(sectionTitle).getTopLeft();
        layOutColumns(pointers, rowsFor(getHeight()), &origin);
    }

protected:
    juce::String title;

private:
    std::vector<std::unique_ptr<ParameterControl>> items;
    std::vector<ParameterControl*> pointers;
};

// --- Macros and variations ---------------------------------------------------------------------------

class DevicePanel::MacroSection final : public Section
{
public:
    MacroSection(EditorContext& c, juce::ValueTree r) : Section("Macros"), context(c), rack(std::move(r))
    {
        count.setSliderStyle(juce::Slider::IncDecButtons);
        count.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 34, 20);
        count.setRange(0, RackDevice::maxMacros, 1);
        count.setTooltip("Macro count (0-64)");
        count.setWantsKeyboardFocus(false);
        count.onValueChange = [this] {
            const int n = static_cast<int>(count.getValue());
            if (n == racks::macroCount(rack))
                return;
            context.undo().beginNewTransaction("Macro count");
            racks::setMacroCount(context.set(), rack, n);
        };
        addAndMakeVisible(count);

        variations.setTextWhenNothingSelected("Variations");
        variations.setWantsKeyboardFocus(false);
        variations.onChange = [this] {
            const int index = variations.getSelectedId() - 1;
            if (updating || index < 0)
                return;
            context.undo().beginNewTransaction("Recall variation");
            racks::recallVariation(context.set(), rack, index);
        };
        addAndMakeVisible(variations);
        store.setTooltip("Store the macros as a variation");
        store.setWantsKeyboardFocus(false);
        store.onClick = [this] {
            askForText("Store variation", "Name", "Variation " + juce::String(racks::numVariations(rack) + 1),
                        [safe = juce::Component::SafePointer<MacroSection>(this)](const juce::String& name) {
                            if (safe == nullptr)
                                return;
                            safe->context.undo().beginNewTransaction("Store variation");
                            racks::storeVariation(safe->context.set(), safe->rack, name);
                        });
        };
        addAndMakeVisible(store);
        refresh();
    }

    void refresh()
    {
        const juce::ScopedValueSetter<bool> guard(updating, true);
        count.setValue(racks::macroCount(rack), juce::dontSendNotification);
        variations.clear(juce::dontSendNotification);
        for (int i = 0; i < racks::numVariations(rack); ++i)
            variations.addItem(racks::variationName(rack, i), i + 1);
        for (const auto& c : controls())
            c->refresh();
        repaint();
    }

    int preferredWidth(int height) const override
    {
        // The knobs go below the count and variations bar.
        return std::max(layOutColumns(controls(), rowsFor(height - barHeight), nullptr) + 2 * sectionPad, 220);
    }

    void resized() override
    {
        auto area = getLocalBounds().reduced(sectionPad, 0).withTrimmedTop(sectionTitle);
        auto bar = area.removeFromTop(barHeight);
        count.setBounds(bar.removeFromLeft(80));
        store.setBounds(bar.removeFromRight(46).reduced(1, 1));
        variations.setBounds(bar.reduced(2, 1));
        auto origin = area.getTopLeft();
        layOutColumns(controls(), rowsFor(getHeight() - barHeight), &origin);
    }

    static constexpr int barHeight = 24;

private:
    EditorContext& context;
    juce::ValueTree rack;
    juce::Slider count;
    juce::ComboBox variations;
    juce::TextButton store{"Store"};
    bool updating = false;
};

// --- Chain list (Instrument and Audio Effect Racks) -------------------------------------------------------

class DevicePanel::ChainList final : public juce::Component
{
public:
    ChainList(EditorContext& c, juce::ValueTree r, std::function<void(const juce::ValueTree&)> select)
        : context(c), rack(std::move(r)), onSelect(std::move(select))
    {
        add.setTooltip("Add a chain");
        add.setWantsKeyboardFocus(false);
        add.onClick = [this] {
            context.undo().beginNewTransaction("Add chain");
            const auto chain = racks::addChain(context.set(), rack,
                                               "Chain " + juce::String(chains().getNumChildren() + 1));
            onSelect(chain);
        };
        addAndMakeVisible(add);
    }

    void setSelected(const juce::ValueTree& chain)
    {
        selected = chain;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        drawSectionFrame(g, getLocalBounds(), "Chains");
        auto area = rowsArea();
        g.setFont(juce::FontOptions(12.0f));
        for (const auto& chain : chains())
        {
            auto row = area.removeFromTop(rowHeight);
            if (row.getHeight() < rowHeight)
                break;
            g.setColour(chain == selected ? colours::emptySlot.brighter(0.25f) : colours::emptySlot);
            g.fillRect(row.reduced(0, 1));
            auto r = row.reduced(4, 0);
            // M S | volume | name | zones
            const bool mute = chain[ids::mute], solo = chain[ids::solo];
            auto m = r.removeFromLeft(18);
            g.setColour(mute ? colours::mute : colours::dimText.darker(0.4f));
            g.drawText("M", m, juce::Justification::centred);
            auto s = r.removeFromLeft(18);
            g.setColour(solo ? colours::solo : colours::dimText.darker(0.4f));
            g.drawText("S", s, juce::Justification::centred);
            const double db = chain.getProperty(ids::volumeDb, 0.0);
            g.setColour(colours::dimText);
            g.drawText(juce::String(db, 1), r.removeFromRight(40), juce::Justification::centredRight);
            g.setColour(colours::text);
            g.drawText(chain[ids::name].toString(), r.reduced(4, 0), juce::Justification::centredLeft, true);
        }
    }

    void resized() override { add.setBounds(getLocalBounds().removeFromTop(sectionTitle).removeFromRight(22).reduced(1)); }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const auto chain = chainAt(e.getPosition());
        if (!chain.isValid())
            return;
        const int x = e.x - rowsArea().getX() - 4;
        if (e.mods.isPopupMenu())
        {
            showMenu(chain);
            return;
        }
        if (x < 18)
        {
            context.undo().beginNewTransaction("Chain mute");
            racks::setChainMix(context.set(), chain, chain.getProperty(ids::volumeDb, 0.0), chain.getProperty(ids::pan, 0.0),
                               !static_cast<bool>(chain[ids::mute]), chain[ids::solo]);
            return;
        }
        if (x < 36)
        {
            context.undo().beginNewTransaction("Chain solo");
            racks::setChainMix(context.set(), chain, chain.getProperty(ids::volumeDb, 0.0), chain.getProperty(ids::pan, 0.0),
                               chain[ids::mute], !static_cast<bool>(chain[ids::solo]));
            return;
        }
        onSelect(chain);
        dragStartDb = chain.getProperty(ids::volumeDb, 0.0);
        dragChain = chain;
        draggingVolume = e.x > rowsArea().getRight() - 48;
        if (draggingVolume)
            context.undo().beginNewTransaction("Chain volume");
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (!draggingVolume || !dragChain.isValid())
            return;
        const double db = juce::jlimit(LiveSet::minVolumeDb, LiveSet::maxVolumeDb,
                                       dragStartDb - e.getDistanceFromDragStartY() * 0.25);
        racks::setChainMix(context.set(), dragChain, db, dragChain.getProperty(ids::pan, 0.0), dragChain[ids::mute],
                           dragChain[ids::solo]);
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        draggingVolume = false;
        dragChain = {};
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        if (const auto chain = chainAt(e.getPosition()); chain.isValid() && e.x < rowsArea().getRight() - 48)
            rename(chain);
    }

private:
    static constexpr int rowHeight = 22;

    juce::ValueTree chains() const { return rack.getChildWithName(ids::rackChains); }
    juce::Rectangle<int> rowsArea() const { return getLocalBounds().reduced(sectionPad, 0).withTrimmedTop(sectionTitle); }

    juce::ValueTree chainAt(juce::Point<int> p) const
    {
        const auto area = rowsArea();
        if (!area.contains(p))
            return {};
        const int index = (p.y - area.getY()) / rowHeight;
        return chains().getChild(index);
    }

    void rename(juce::ValueTree chain)
    {
        askForText("Rename chain", "Name", chain[ids::name].toString(),
                   [this, chain](const juce::String& name) mutable {
                       context.undo().beginNewTransaction("Rename chain");
                       chain.setProperty(ids::name, name, context.set().undoManager());
                   });
    }

    /** "low-high fadeLow fadeHigh", e.g. "36-60 0 0". */
    static juce::String zoneText(const juce::ValueTree& chain, bool key)
    {
        const auto lo = key ? ids::keyLow : ids::velocityLow;
        const auto hi = key ? ids::keyHigh : ids::velocityHigh;
        const auto fl = key ? ids::keyFadeLow : ids::velocityFadeLow;
        const auto fh = key ? ids::keyFadeHigh : ids::velocityFadeHigh;
        return juce::String(static_cast<int>(chain.getProperty(lo, key ? 0 : 1))) + "-" +
               juce::String(static_cast<int>(chain.getProperty(hi, 127))) + " " +
               juce::String(static_cast<int>(chain.getProperty(fl, 0))) + " " +
               juce::String(static_cast<int>(chain.getProperty(fh, 0)));
    }

    void editZone(const juce::ValueTree& chain, bool key)
    {
        askForText(key ? "Key zone" : "Velocity zone", "low-high fadeLow fadeHigh", zoneText(chain, key),
                   [this, chain, key](const juce::String& text) {
                       const auto parts = juce::StringArray::fromTokens(text.replaceCharacter('-', ' '), " ", {});
                       if (parts.size() < 2)
                           return;
                       const int lo = juce::jlimit(0, 127, parts[0].getIntValue());
                       const int hi = juce::jlimit(lo, 127, parts[1].getIntValue());
                       const int fl = parts.size() > 2 ? std::max(0, parts[2].getIntValue()) : 0;
                       const int fh = parts.size() > 3 ? std::max(0, parts[3].getIntValue()) : 0;
                       context.undo().beginNewTransaction(key ? "Key zone" : "Velocity zone");
                       if (key)
                           racks::setKeyZone(context.set(), chain, lo, hi, fl, fh);
                       else
                           racks::setVelocityZone(context.set(), chain, lo, hi, fl, fh);
                   });
    }

    void showMenu(const juce::ValueTree& chain)
    {
        juce::PopupMenu menu;
        menu.addItem("Rename...", [this, chain] { rename(chain); });
        menu.addItem("Key zone (" + zoneText(chain, true) + ")...", [this, chain] { editZone(chain, true); });
        menu.addItem("Velocity zone (" + zoneText(chain, false) + ")...", [this, chain] { editZone(chain, false); });
        menu.addItem("Pan centre", [this, chain] {
            context.undo().beginNewTransaction("Chain pan");
            racks::setChainMix(context.set(), chain, chain.getProperty(ids::volumeDb, 0.0), 0.0, chain[ids::mute],
                               chain[ids::solo]);
        });
        menu.addSeparator();
        menu.addItem("Delete chain", [this, chain] {
            context.undo().beginNewTransaction("Delete chain");
            racks::removeChain(context.set(), chain);
        });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
    }

    EditorContext& context;
    juce::ValueTree rack;
    std::function<void(const juce::ValueTree&)> onSelect;
    juce::ValueTree selected;
    juce::TextButton add{"+"};
    juce::ValueTree dragChain;
    double dragStartDb = 0.0;
    bool draggingVolume = false;
};

// --- Drum Rack pads -------------------------------------------------------------------------------

class DevicePanel::PadGrid final : public juce::Component, public juce::DragAndDropTarget, public juce::FileDragAndDropTarget
{
public:
    PadGrid(EditorContext& c, juce::ValueTree r, std::function<void(const juce::ValueTree&)> select)
        : context(c), rack(std::move(r)), onSelect(std::move(select))
    {
        for (auto* b : {&up, &down})
        {
            b->setWantsKeyboardFocus(false);
            addAndMakeVisible(*b);
        }
        up.onClick = [this] { setPage(page + 1); };
        down.onClick = [this] { setPage(page - 1); };
        up.setTooltip("Next 16 pads");
        down.setTooltip("Previous 16 pads");
    }

    void setSelected(const juce::ValueTree& chain)
    {
        selected = chain;
        repaint();
    }

    void paint(juce::Graphics& g) override
    {
        drawSectionFrame(g, getLocalBounds(), "Pads " + juce::String(firstNote()) + "-" + juce::String(firstNote() + 15));
        g.setFont(juce::FontOptions(11.0f));
        for (int i = 0; i < 16; ++i)
        {
            const int note = firstNote() + i;
            if (note > 127)
                continue;
            const auto r = padBounds(i);
            const auto pad = racks::padFor(rack, note);
            juce::Colour fill = pad.isValid() ? colours::emptySlot.brighter(0.15f) : colours::emptySlot;
            if (pad.isValid() && pad == selected)
                fill = colours::solo.darker(0.5f);
            if (note == heldNote)
                fill = colours::play.darker(0.3f);
            if (i == hoverPad)
                fill = fill.brighter(0.2f);
            g.setColour(fill);
            g.fillRoundedRectangle(r.toFloat().reduced(2.0f), 3.0f);
            g.setColour(colours::dimText);
            g.drawText(juce::String(note), r.reduced(4, 2), juce::Justification::topLeft);
            if (pad.isValid())
            {
                g.setColour(colours::text);
                g.drawFittedText(pad[ids::name].toString(), r.reduced(4, 2).withTrimmedTop(12),
                                 juce::Justification::centred, 2);
                if (const int choke = pad.getProperty(ids::chokeGroup, 0); choke > 0)
                {
                    g.setColour(colours::mute);
                    g.drawText("c" + juce::String(choke), r.reduced(4, 2), juce::Justification::topRight);
                }
            }
        }
    }

    void resized() override
    {
        auto bar = getLocalBounds().removeFromTop(sectionTitle);
        up.setBounds(bar.removeFromRight(22).reduced(1));
        down.setBounds(bar.removeFromRight(22).reduced(1));
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const int i = padAt(e.getPosition());
        if (i < 0)
            return;
        const int note = firstNote() + i;
        const auto pad = racks::padFor(rack, note);
        if (e.mods.isPopupMenu())
        {
            showMenu(note);
            return;
        }
        if (pad.isValid())
            onSelect(pad);
        // Play it (the track is armed for it, as Live's selection arm would).
        armOwnerTrack(context, rack);
        heldNote = note;
        const auto velocity = static_cast<juce::uint8>(juce::jlimit(1, 127, 127 - (e.y - padBounds(i).getY()) * 2));
        context.engine.midiInput().push(juce::MidiMessage::noteOn(1, note, velocity));
        repaint();
    }

    void mouseUp(const juce::MouseEvent&) override
    {
        if (heldNote >= 0)
            context.engine.midiInput().push(juce::MidiMessage::noteOff(1, heldNote));
        heldNote = -1;
        repaint();
    }

    bool isInterestedInDragSource(const SourceDetails& d) override { return drag::is(d.description, "sample"); }
    void itemDragMove(const SourceDetails& d) override { setHover(padAt(d.localPosition)); }
    void itemDragExit(const SourceDetails&) override { setHover(-1); }
    void itemDropped(const SourceDetails& d) override
    {
        dropSample(padAt(d.localPosition), juce::File(drag::payload(d.description)));
        setHover(-1);
    }
    bool isInterestedInFileDrag(const juce::StringArray& files) override
    {
        return std::any_of(files.begin(), files.end(), [](const juce::String& f) { return isSampleFile(juce::File(f)); });
    }
    void fileDragMove(const juce::StringArray&, int x, int y) override { setHover(padAt({x, y})); }
    void fileDragExit(const juce::StringArray&) override { setHover(-1); }
    void filesDropped(const juce::StringArray& files, int x, int y) override
    {
        // Several files fill consecutive pads from the one they were dropped on.
        int i = padAt({x, y});
        if (i < 0)
            return;
        context.undo().beginNewTransaction("Load samples");
        for (const auto& f : files)
            if (isSampleFile(juce::File(f)) && firstNote() + i <= 127)
                assign(firstNote() + i++, juce::File(f));
        setHover(-1);
    }

private:
    int firstNote() const { return juce::jlimit(0, 112, 36 + 16 * page); }
    void setPage(int p)
    {
        page = juce::jlimit(-2, 5, p); // notes 4..127 in pages of 16
        repaint();
    }
    juce::Rectangle<int> padArea() const { return getLocalBounds().reduced(sectionPad, 0).withTrimmedTop(sectionTitle).withTrimmedBottom(4); }
    juce::Rectangle<int> padBounds(int i) const
    {
        // Pad 0 is bottom left, as on Push and in Live.
        const auto area = padArea();
        const int w = area.getWidth() / 4, h = area.getHeight() / 4;
        return {area.getX() + (i % 4) * w, area.getBottom() - (i / 4 + 1) * h, w, h};
    }
    int padAt(juce::Point<int> p) const
    {
        for (int i = 0; i < 16; ++i)
            if (padBounds(i).contains(p))
                return i;
        return -1;
    }
    void setHover(int i)
    {
        if (i != hoverPad)
        {
            hoverPad = i;
            repaint();
        }
    }

    /** A sample on a note: a new pad, or a new sample in the pad's Sampler. */
    void assign(int note, const juce::File& file)
    {
        auto pad = racks::padFor(rack, note);
        if (!pad.isValid())
        {
            pad = racks::addDrumPad(context.set(), rack, note, file.getFullPathName());
            pad.setProperty(ids::name, file.getFileNameWithoutExtension(), context.set().undoManager());
            onSelect(pad);
            return;
        }
        for (auto d : racks::devicesOf(pad))
            if (d[ids::deviceType].toString() == samplerType)
            {
                d.getChildWithName(ids::deviceState).setProperty("file", file.getFullPathName(), context.set().undoManager());
                pad.setProperty(ids::name, file.getFileNameWithoutExtension(), context.set().undoManager());
                return;
            }
    }

    void dropSample(int i, const juce::File& file)
    {
        if (i < 0 || !isSampleFile(file))
            return;
        context.undo().beginNewTransaction("Load sample");
        assign(firstNote() + i, file);
    }

    void showMenu(int note)
    {
        juce::PopupMenu menu;
        const auto pad = racks::padFor(rack, note);
        menu.addSectionHeader("Pad " + juce::String(note));
        menu.addItem("Load sample...", [this, note] {
            context.chooseFile("Load a sample for pad " + juce::String(note), "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3",
                               [safe = juce::Component::SafePointer<PadGrid>(this), note](const juce::File& f) {
                                   if (safe == nullptr)
                                       return;
                                   safe->context.undo().beginNewTransaction("Load sample");
                                   safe->assign(note, f);
                               });
        });
        if (!pad.isValid())
            menu.addItem("Add empty pad", [this, note] {
                context.undo().beginNewTransaction("Add pad");
                onSelect(racks::addDrumPad(context.set(), rack, note));
            });
        else
        {
            juce::PopupMenu choke;
            const int current = pad.getProperty(ids::chokeGroup, 0);
            for (int group = 0; group <= 16; ++group)
                choke.addItem(group == 0 ? juce::String("None") : juce::String(group), true, group == current,
                              [this, pad, group] {
                                  context.undo().beginNewTransaction("Choke group");
                                  racks::setChokeGroup(context.set(), pad, group);
                              });
            menu.addSubMenu("Choke group", choke);
            menu.addItem("Remove pad", [this, pad] {
                context.undo().beginNewTransaction("Remove pad");
                racks::removeChain(context.set(), pad);
            });
        }
        menu.addSeparator();
        const bool tuned = rack.getChildWithName(ids::deviceState).getProperty("tunedPads", false);
        menu.addItem("Tuned pads (follow the track's tuning)", true, tuned, [this, tuned] {
            context.undo().beginNewTransaction("Tuned pads");
            racks::setTunedPads(context.set(), rack, !tuned);
        });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this));
    }

    EditorContext& context;
    juce::ValueTree rack;
    std::function<void(const juce::ValueTree&)> onSelect;
    juce::ValueTree selected;
    juce::TextButton up{juce::String::fromUTF8("\xe2\x96\xb2")};   // ▲
    juce::TextButton down{juce::String::fromUTF8("\xe2\x96\xbc")}; // ▼
    int page = 0;
    int heldNote = -1;
    int hoverPad = -1;
};

// --- Sidechain source --------------------------------------------------------------------------------

class DevicePanel::SidechainSection final : public juce::Component
{
public:
    SidechainSection(EditorContext& c, juce::ValueTree d) : context(c), device(std::move(d))
    {
        source.setWantsKeyboardFocus(false);
        source.onChange = [this] {
            if (updating)
                return;
            const int index = source.getSelectedId() - 2; // 1 = none
            const auto id = index >= 0 && index < static_cast<int>(choices.size()) ? choices[static_cast<std::size_t>(index)] : 0;
            context.undo().beginNewTransaction("Sidechain source");
            chains::setSidechainSource(context.set(), device, id, preFx.getToggleState());
        };
        preFx.setClickingTogglesState(false);
        preFx.setTooltip("Take the source before its effects (after its instrument)");
        preFx.setWantsKeyboardFocus(false);
        preFx.onClick = [this] {
            context.undo().beginNewTransaction("Sidechain tap");
            chains::setSidechainSource(context.set(), device, currentSource(), !preFx.getToggleState());
        };
        addAndMakeVisible(source);
        addAndMakeVisible(preFx);
        refresh();
    }

    void refresh()
    {
        const juce::ScopedValueSetter<bool> guard(updating, true);
        // Offer only sources that can't form a loop (and whatever is set now).
        choices = chains::sidechainSources(context.set(), device);
        const auto current = currentSource();
        if (current != 0 && std::find(choices.begin(), choices.end(), current) == choices.end())
            choices.push_back(current);
        source.clear(juce::dontSendNotification);
        source.addItem("No source", 1);
        int selectedId = 1;
        for (std::size_t i = 0; i < choices.size(); ++i)
        {
            const int t = context.set().trackIndexForId(choices[i]);
            source.addItem(t >= 0 ? context.set().track(t)[ids::name].toString() : "?", static_cast<int>(i) + 2);
            if (choices[i] == current)
                selectedId = static_cast<int>(i) + 2;
        }
        source.setSelectedId(selectedId, juce::dontSendNotification);
        preFx.setToggleState(device.getChildWithName(ids::deviceState).getProperty(ids::sidechainPreFx, false),
                             juce::dontSendNotification);
    }

    void paint(juce::Graphics& g) override { drawSectionFrame(g, getLocalBounds(), "Sidechain from"); }
    void resized() override
    {
        auto area = getLocalBounds().reduced(sectionPad, 0).withTrimmedTop(sectionTitle);
        source.setBounds(area.removeFromTop(24).reduced(0, 1));
        area.removeFromTop(4);
        preFx.setBounds(area.removeFromTop(24).reduced(0, 1));
    }

private:
    std::int64_t currentSource() const
    {
        return static_cast<juce::int64>(device.getChildWithName(ids::deviceState).getProperty(ids::sidechainTrack, 0));
    }

    EditorContext& context;
    juce::ValueTree device;
    juce::ComboBox source;
    juce::TextButton preFx{"Pre FX"};
    std::vector<std::int64_t> choices;
    bool updating = false;
};

// --- Sampler file --------------------------------------------------------------------------------------

class DevicePanel::SampleSection final : public juce::Component
{
public:
    SampleSection(std::function<void()> load, juce::ValueTree d) : device(std::move(d))
    {
        loadButton.onClick = std::move(load);
        loadButton.setWantsKeyboardFocus(false);
        addAndMakeVisible(loadButton);
    }

    void paint(juce::Graphics& g) override
    {
        drawSectionFrame(g, getLocalBounds(), "Sample");
        const juce::File file(device.getChildWithName(ids::deviceState)["file"].toString());
        auto area = getLocalBounds().reduced(sectionPad, 0).withTrimmedTop(sectionTitle + 30);
        g.setColour(file.getFullPathName().isNotEmpty() ? colours::text : colours::dimText);
        g.setFont(juce::FontOptions(12.0f));
        g.drawFittedText(file.getFullPathName().isNotEmpty() ? file.getFileName() : "Drop a sample here",
                         area.removeFromTop(40), juce::Justification::centredLeft, 3);
    }

    void resized() override
    {
        loadButton.setBounds(getLocalBounds().reduced(sectionPad, 0).withTrimmedTop(sectionTitle).removeFromTop(24));
    }

private:
    juce::ValueTree device;
    juce::TextButton loadButton{"Load sample..."};
};

// --- DevicePanel ------------------------------------------------------------------------------------

DevicePanel::DevicePanel(EditorContext& c, juce::ValueTree device, int height)
    : context(c), node(std::move(device)), panelHeight(height)
{
    const auto type = node[ids::deviceType].toString();
    rack = RackDevice::isRackType(type);
    drumRack = rack && RackDevice::kindForType(type) == RackKind::Drum;

    enable.setClickingTogglesState(false);
    enable.setColour(juce::TextButton::buttonOnColourId, colours::mute);
    enable.setTooltip("Device on/off");
    enable.onClick = [this] {
        context.undo().beginNewTransaction("Device on/off");
        context.set().setDeviceEnabled(node, !static_cast<bool>(node.getProperty(ids::enabled, true)));
    };
    title.setText(deviceName(context, node), juce::dontSendNotification);
    title.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    title.setColour(juce::Label::textColourId, colours::text);
    title.setInterceptsMouseClicks(false, false);
    presetButton.setTooltip("Presets");
    presetButton.onClick = [this] { showPresetMenu(); };
    remove.setTooltip("Delete the device");
    remove.onClick = [this] {
        context.undo().beginNewTransaction("Delete device");
        context.set().removeDevice(node); // the chain row rebuilds without this panel
    };
    for (auto* b : {&enable, &presetButton, &remove})
    {
        b->setWantsKeyboardFocus(false);
        addAndMakeVisible(*b);
    }
    addAndMakeVisible(title);

    build();
    node.addListener(this);
}

DevicePanel::~DevicePanel()
{
    cancelPendingUpdate();
    node.removeListener(this);
}

bool DevicePanel::isSampler() const
{
    return node[ids::deviceType].toString() == samplerType;
}

std::vector<juce::ValueTree> DevicePanel::enclosingRacks() const
{
    std::vector<juce::ValueTree> out;
    for (auto n = node.getParent(); n.isValid(); n = n.getParent())
        if (n.hasType(ids::device) && RackDevice::isRackType(n[ids::deviceType].toString()))
            out.push_back(n);
    return out;
}

void DevicePanel::addMappingItems(juce::PopupMenu& menu, const juce::String& parameterId)
{
    const auto deviceId = LiveSet::idOf(node);
    for (const auto& r : enclosingRacks())
    {
        const int count = racks::macroCount(r);
        juce::PopupMenu sub;
        for (int m = 0; m < count; ++m)
            sub.addItem(racks::macroName(r, m), [this, r, m, parameterId] {
                context.undo().beginNewTransaction("Map to macro");
                racks::addMapping(context.set(), r, m, node, parameterId);
            });
        if (count == 0)
            sub.addItem("(the rack has no macros)", false, false, nullptr);
        menu.addSubMenu("Map to " + deviceName(context, r) + " macro", sub);
        for (const auto& mapping : racks::mappingsOf(r))
            if (static_cast<juce::int64>(mapping[ids::targetDevice]) == deviceId && mapping[ids::parameter].toString() == parameterId)
                menu.addItem("Remove mapping from " + racks::macroName(r, mapping[ids::macro]), [this, mapping] {
                    context.undo().beginNewTransaction("Remove mapping");
                    racks::removeMapping(context.set(), mapping);
                });
    }
}

void DevicePanel::build()
{
    sections.clear();
    controls.clear();
    innerRow.reset();
    innerView.reset();

    const auto* prototype = context.specs.forType(node[ids::deviceType].toString());
    const int bodyHeight = panelHeight - headerHeight;
    if (prototype == nullptr)
        return;

    const bool mappable = !enclosingRacks().empty();
    const auto makeControl = [&](int index) {
        const auto& p = prototype->parameter(index);
        auto control = std::make_unique<ParameterControl>(context.session, node, p);
        if (mappable && p.spec().modulatable)
            control->extendMenu = [this, id = p.spec().id](juce::PopupMenu& menu) {
                menu.addSeparator();
                addMappingItems(menu, id);
            };
        controls.push_back(control.get());
        return control;
    };

    if (rack)
    {
        // Macros first (only as many as the rack uses), named; then chains or pads, then the
        // selected chain's devices.
        auto macros = std::make_unique<MacroSection>(context, node);
        const int count = racks::macroCount(node);
        for (int m = 0; m < count; ++m)
        {
            const int index = prototype->parameterIndex("macro" + juce::String(m + 1));
            if (index < 0)
                continue;
            auto control = makeControl(index);
            control->setDisplayName(racks::macroName(node, m));
            control->extendMenu = [this, m](juce::PopupMenu& menu) {
                menu.addSeparator();
                menu.addItem("Rename macro...", [this, m] {
                    askForText("Rename macro", "Name", racks::macroName(node, m), [this, m](const juce::String& name) {
                        context.undo().beginNewTransaction("Rename macro");
                        racks::setMacroName(context.set(), node, m, name);
                    });
                });
                for (const auto& mapping : racks::mappingsOf(node, m))
                {
                    const auto target = context.set().deviceById(static_cast<juce::int64>(mapping[ids::targetDevice]));
                    menu.addItem("Unmap " + deviceName(context, target) + " " + mapping[ids::parameter].toString(),
                                 [this, mapping] {
                                     context.undo().beginNewTransaction("Remove mapping");
                                     racks::removeMapping(context.set(), mapping);
                                 });
                }
            };
            macros->add(std::move(control));
        }
        sections.push_back(std::move(macros));

        const auto select = [this](const juce::ValueTree& chain) { selectChain(chain); };
        if (drumRack)
            sections.push_back(std::make_unique<PadGrid>(context, node, select));
        else
            sections.push_back(std::make_unique<ChainList>(context, node, select));
        sections.back()->getProperties().set("width", drumRack ? padGridWidth : chainListWidth);

        // Keep the selected chain if it still exists; otherwise the first one.
        const auto chainsNode = node.getChildWithName(ids::rackChains);
        if (!selectedChain.isValid() || !selectedChain.isAChildOf(chainsNode))
            selectedChain = drumRack ? juce::ValueTree() : chainsNode.getChild(0);
        if (selectedChain.isValid())
        {
            innerRow = std::make_unique<ChainRow>(context, racks::devicesOf(selectedChain), bodyHeight - 10);
            innerRow->onWidthChanged = [this] {
                resized();
                if (onWidthChanged)
                    onWidthChanged();
            };
            innerView = std::make_unique<juce::Viewport>();
            innerView->setViewedComponent(innerRow.get(), false);
            innerView->setScrollBarsShown(false, true);
            addAndMakeVisible(*innerView);
        }
    }
    else
    {
        for (const auto& group : prototype->parameterGroups())
        {
            auto section = std::make_unique<Section>(group.name);
            for (const int index : group.parameters)
                section->add(makeControl(index));
            sections.push_back(std::move(section));
        }
        // Device extras.
        for (int i = 0; i < prototype->numParameters(); ++i)
            if (prototype->parameter(i).spec().id.endsWith(".sidechain"))
            {
                sections.push_back(std::make_unique<SidechainSection>(context, node));
                sections.back()->getProperties().set("width", sidechainWidth);
                break;
            }
        if (isSampler())
            sections.insert(sections.begin(), std::make_unique<SampleSection>(
                                                  [this] {
                                                      context.chooseFile("Load a sample", "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3",
                                                                         [safe = juce::Component::SafePointer<DevicePanel>(this)](const juce::File& f) {
                                                                             if (safe != nullptr)
                                                                                 safe->loadSample(f);
                                                                         });
                                                  },
                                                  node));
        if (isSampler())
            sections.front()->getProperties().set("width", sampleWidth);
    }
    for (auto& s : sections)
        addAndMakeVisible(*s);
    refresh();
    setSize(preferredWidth(), panelHeight);
    resized();
}

void DevicePanel::setPanelHeight(int height)
{
    if (height == panelHeight)
        return;
    panelHeight = height;
    if (innerRow != nullptr)
        innerRow->setRowHeight(height - headerHeight - 10);
    setSize(preferredWidth(), height);
    resized();
}

void DevicePanel::selectChain(const juce::ValueTree& chain)
{
    if (chain == selectedChain && innerRow != nullptr)
        return;
    selectedChain = chain;
    build();
    if (onWidthChanged)
        onWidthChanged();
}

void DevicePanel::refresh()
{
    const bool on = node.getProperty(ids::enabled, true);
    enable.setToggleState(on, juce::dontSendNotification);
    for (auto* c : controls)
        c->refresh();
    for (auto& s : sections)
    {
        if (auto* m = dynamic_cast<MacroSection*>(s.get()))
            m->refresh();
        else if (auto* side = dynamic_cast<SidechainSection*>(s.get()))
            side->refresh();
        else if (auto* list = dynamic_cast<ChainList*>(s.get()))
            list->setSelected(selectedChain);
        else if (auto* pads = dynamic_cast<PadGrid*>(s.get()))
            pads->setSelected(selectedChain);
        else
            s->repaint();
    }
    repaint();
}

int DevicePanel::sectionWidth(const juce::Component& s, int bodyHeight)
{
    if (const auto* section = dynamic_cast<const Section*>(&s))
        return section->preferredWidth(bodyHeight);
    return s.getProperties().getWithDefault("width", 160); // a fixed-width section
}

int DevicePanel::preferredWidth() const
{
    const int bodyHeight = panelHeight - headerHeight - 2;
    int w = 8;
    for (const auto& s : sections)
        w += sectionWidth(*s, bodyHeight);
    if (innerRow != nullptr)
        w += innerRow->preferredWidth() + 8;
    return std::max(w, 160);
}

void DevicePanel::paint(juce::Graphics& g)
{
    const bool on = node.getProperty(ids::enabled, true);
    g.setColour(colours::panel);
    g.fillRoundedRectangle(getLocalBounds().toFloat().reduced(1.0f), 4.0f);
    g.setColour(on ? colours::panel.brighter(0.15f) : colours::panel.darker(0.2f));
    g.fillRoundedRectangle(getLocalBounds().removeFromTop(headerHeight).toFloat().reduced(1.0f), 4.0f);
    if (innerRow != nullptr && innerView != nullptr)
    {
        g.setColour(colours::background);
        g.fillRect(innerView->getBounds().expanded(2));
    }
    if (!on)
    {
        g.setColour(juce::Colours::black.withAlpha(0.25f));
        g.fillRect(getLocalBounds().withTrimmedTop(headerHeight));
    }
}

void DevicePanel::resized()
{
    auto area = getLocalBounds().reduced(2, 0);
    auto header = area.removeFromTop(headerHeight).reduced(2);
    enable.setBounds(header.removeFromLeft(22));
    remove.setBounds(header.removeFromRight(22));
    presetButton.setBounds(header.removeFromRight(22));
    title.setBounds(header.reduced(4, 0));

    area.removeFromTop(2);
    for (auto& s : sections)
        s->setBounds(area.removeFromLeft(sectionWidth(*s, area.getHeight())));
    if (innerRow != nullptr && innerView != nullptr)
    {
        area.removeFromLeft(6);
        innerView->setBounds(area.reduced(0, 2));
        innerRow->setSize(std::max(innerRow->preferredWidth(), innerView->getWidth()), innerView->getHeight());
    }
}

void DevicePanel::mouseDown(const juce::MouseEvent& e)
{
    if (e.y < headerHeight && e.mods.isPopupMenu())
        showPresetMenu();
}

void DevicePanel::mouseDrag(const juce::MouseEvent& e)
{
    // Dragging the header moves the device.
    if (e.getMouseDownY() >= headerHeight || e.getDistanceFromDragStart() < 6)
        return;
    if (auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this);
        container != nullptr && !container->isDragAndDropActive())
    {
        const auto image = createComponentSnapshot(getLocalBounds().removeFromTop(headerHeight));
        container->startDragging(drag::move(LiveSet::idOf(node)), this, juce::ScaledImage(image));
    }
}

void DevicePanel::showPresetMenu()
{
    juce::PopupMenu menu;
    const auto type = node[ids::deviceType].toString();
    menu.addItem("Save as preset...", [this] {
        askForText("Save preset", "Name", deviceName(context, node), [this](const juce::String& name) {
            juce::File written;
            const auto result = context.presets.saveUserPreset(node, name, written);
            if (result.failed())
                juce::AlertWindow::showMessageBoxAsync(juce::MessageBoxIconType::WarningIcon, "Could not save the preset",
                                                       result.getErrorMessage());
        });
    });
    const auto list = context.presets.presetsFor(type);
    if (!list.empty())
    {
        menu.addSeparator();
        menu.addSectionHeader("Replace with preset");
        for (const auto& info : list)
            menu.addItem(info.name + (info.factory ? "" : "  (user)"), [this, info] {
                presets::Preset p;
                if (context.presets.load(info, p).failed())
                    return;
                // The preset takes this device's place.
                auto chain = node.getParent();
                const int index = chain.indexOf(node);
                context.undo().beginNewTransaction("Load preset");
                context.set().removeDevice(node);
                context.set().insertDeviceCopy(chain, index, p.device);
            });
    }
    menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(&presetButton));
}

void DevicePanel::loadSample(const juce::File& file)
{
    if (!isSampleFile(file))
        return;
    context.undo().beginNewTransaction("Load sample");
    auto state = node.getChildWithName(ids::deviceState);
    state.setProperty("file", file.getFullPathName(), context.set().undoManager());
}

bool DevicePanel::isInterestedInDragSource(const SourceDetails& details)
{
    return isSampler() && drag::is(details.description, "sample");
}

void DevicePanel::itemDropped(const SourceDetails& details)
{
    loadSample(juce::File(drag::payload(details.description)));
}

bool DevicePanel::isInterestedInFileDrag(const juce::StringArray& files)
{
    return isSampler() && files.size() == 1 && isSampleFile(juce::File(files[0]));
}

void DevicePanel::filesDropped(const juce::StringArray& files, int, int)
{
    loadSample(juce::File(files[0]));
}

void DevicePanel::needsRebuild()
{
    triggerAsyncUpdate();
}

void DevicePanel::handleAsyncUpdate()
{
    const int before = preferredWidth();
    build();
    if (preferredWidth() != before && onWidthChanged)
        onWidthChanged();
}

void DevicePanel::flushUpdates()
{
    handleUpdateNowIfNeeded();
    if (innerRow != nullptr)
        innerRow->flushUpdates();
}

void DevicePanel::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    // Only this device's own nodes: nested devices have their own panels.
    if (tree == node || tree.getParent() == node)
    {
        if (tree.hasType(ids::deviceState) && property == juce::Identifier("macroCount"))
            needsRebuild();
        else
            refresh();
    }
    else if (tree.hasType(ids::rackChain) && tree.getParent().getParent() == node)
        refresh(); // a chain's mix or zones
    else if (tree.hasType("MACRO") && tree.getParent().getParent() == node)
        needsRebuild(); // a macro's name
}

void DevicePanel::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree& child)
{
    if (parent.hasType(ids::rackChains) && parent.getParent() == node)
    {
        selectedChain = child; // a new chain or pad: show it
        needsRebuild();
    }
    else if (child.hasType("MACRO") && parent.getParent() == node)
        needsRebuild(); // a macro named for the first time
    else if (parent == node || parent.getParent() == node)
        refresh();
}

void DevicePanel::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int)
{
    if (parent.hasType(ids::rackChains) && parent.getParent() == node)
        needsRebuild();
    else if (parent == node || parent.getParent() == node)
        refresh();
}

// --- ChainRow ------------------------------------------------------------------------------------------

ChainRow::ChainRow(EditorContext& c, juce::ValueTree devicesNode, int height)
    : context(c), devices(std::move(devicesNode)), rowHeight(height)
{
    devices.addListener(this);
    rebuild();
}

ChainRow::~ChainRow()
{
    cancelPendingUpdate();
    devices.removeListener(this);
}

void ChainRow::flushUpdates()
{
    handleUpdateNowIfNeeded();
    for (auto& p : panels)
        p->flushUpdates();
}

void ChainRow::rebuild()
{
    // Keep the panels of devices that are still here (they keep their selected chains).
    std::vector<std::unique_ptr<DevicePanel>> next;
    for (const auto& d : devices)
    {
        if (!d.hasType(ids::device))
            continue;
        auto it = std::find_if(panels.begin(), panels.end(), [&](const auto& p) { return p != nullptr && p->device() == d; });
        if (it != panels.end())
            next.push_back(std::move(*it));
        else
        {
            auto panel = std::make_unique<DevicePanel>(context, d, rowHeight);
            panel->onWidthChanged = [this] { layout(); };
            addAndMakeVisible(*panel);
            next.push_back(std::move(panel));
        }
    }
    panels = std::move(next);
    layout();
}

int ChainRow::preferredWidth() const
{
    int w = emptyWidth;
    for (const auto& p : panels)
        w += p->preferredWidth() + 4;
    return w;
}

void ChainRow::setRowHeight(int height)
{
    if (height == rowHeight)
        return;
    rowHeight = height;
    for (auto& p : panels)
        p->setPanelHeight(height);
    layout();
}

void ChainRow::layout()
{
    const int before = getWidth();
    setSize(std::max(preferredWidth(), getParentWidth()), rowHeight);
    resized();
    if (getWidth() != before && onWidthChanged)
        onWidthChanged();
}

void ChainRow::resized()
{
    int x = 0;
    for (auto& p : panels)
    {
        p->setBounds(x, 0, p->preferredWidth(), getHeight());
        x += p->getWidth() + 4;
    }
}

void ChainRow::paint(juce::Graphics& g)
{
    const int end = panels.empty() ? 0 : panels.back()->getRight() + 4;
    auto hint = juce::Rectangle<int>(end, 0, std::max(0, getWidth() - end), getHeight()).reduced(6);
    g.setColour(colours::emptySlot);
    g.drawRoundedRectangle(hint.toFloat(), 4.0f, 1.0f);
    g.setColour(colours::dimText);
    g.setFont(juce::FontOptions(12.0f));
    g.drawFittedText(panels.empty() ? "Drop devices, presets or samples here" : "Drop here", hint.reduced(8),
                     juce::Justification::centred, 3);
}

void ChainRow::paintOverChildren(juce::Graphics& g)
{
    if (dropIndex < 0)
        return;
    const int x = dropIndex < static_cast<int>(panels.size()) ? panels[static_cast<std::size_t>(dropIndex)]->getX() - 2
                  : panels.empty()                            ? 2
                                                              : panels.back()->getRight() + 2;
    g.setColour(colours::selection);
    g.fillRect(x - 1, 0, 3, getHeight());
}

int ChainRow::indexAt(int x) const
{
    for (std::size_t i = 0; i < panels.size(); ++i)
        if (x < panels[i]->getX() + panels[i]->getWidth() / 2)
            return static_cast<int>(i);
    return static_cast<int>(panels.size());
}

void ChainRow::insertFromDescription(const juce::String& description, int index)
{
    auto& set = context.set();
    const auto& registry = context.registry();
    const auto payload = drag::payload(description);
    if (drag::is(description, "device"))
    {
        context.undo().beginNewTransaction("Insert device");
        chains::insert(set, devices, payload, index, registry);
    }
    else if (drag::is(description, "preset"))
    {
        const int i = payload.getIntValue();
        const auto& all = context.presets.all();
        if (i < 0 || i >= static_cast<int>(all.size()))
            return;
        presets::Preset p;
        if (context.presets.load(all[static_cast<std::size_t>(i)], p).failed())
            return;
        context.undo().beginNewTransaction("Load preset");
        chains::insertCopy(set, devices, p.device, index, registry);
    }
    else if (drag::is(description, "sample"))
    {
        // A sample on a chain: a Sampler playing it (replacing the instrument).
        const juce::File file(payload);
        if (!isSampleFile(file))
            return;
        context.undo().beginNewTransaction("Load sample");
        auto sampler = chains::insert(set, devices, samplerType, index, registry);
        if (sampler.isValid())
            sampler.getChildWithName(ids::deviceState).setProperty("file", file.getFullPathName(), set.undoManager());
    }
    else if (drag::is(description, "move"))
    {
        const auto device = set.deviceById(payload.getLargeIntValue());
        if (!device.isValid())
            return;
        context.undo().beginNewTransaction("Move device");
        chains::move(set, device, devices, index, registry);
    }
}

bool ChainRow::isInterestedInDragSource(const SourceDetails& details)
{
    const auto& d = details.description;
    if (drag::is(d, "move"))
    {
        // Not into itself (a rack's own chain).
        const auto device = context.set().deviceById(drag::payload(d).getLargeIntValue());
        return device.isValid() && !devices.isAChildOf(device);
    }
    return drag::is(d, "device") || drag::is(d, "preset") || drag::is(d, "sample");
}

void ChainRow::itemDragMove(const SourceDetails& details)
{
    const int index = indexAt(details.localPosition.x);
    if (index != dropIndex)
    {
        dropIndex = index;
        repaint();
    }
}

void ChainRow::itemDragExit(const SourceDetails&)
{
    dropIndex = -1;
    repaint();
}

void ChainRow::itemDropped(const SourceDetails& details)
{
    const int index = indexAt(details.localPosition.x);
    dropIndex = -1;
    repaint();
    insertFromDescription(details.description.toString(), index);
}

bool ChainRow::isInterestedInFileDrag(const juce::StringArray& files)
{
    return std::any_of(files.begin(), files.end(), [](const juce::String& f) {
        const juce::File file(f);
        return isSampleFile(file) || file.hasFileExtension(presets::fileExtension);
    });
}

void ChainRow::fileDragMove(const juce::StringArray&, int x, int)
{
    const int index = indexAt(x);
    if (index != dropIndex)
    {
        dropIndex = index;
        repaint();
    }
}

void ChainRow::fileDragExit(const juce::StringArray&)
{
    dropIndex = -1;
    repaint();
}

void ChainRow::filesDropped(const juce::StringArray& files, int x, int)
{
    int index = indexAt(x);
    dropIndex = -1;
    repaint();
    for (const auto& f : files)
    {
        const juce::File file(f);
        if (file.hasFileExtension(presets::fileExtension))
        {
            presets::Preset p;
            if (presets::read(file, p).failed())
                continue;
            context.undo().beginNewTransaction("Load preset");
            chains::insertCopy(context.set(), devices, p.device, index++, context.registry());
        }
        else if (isSampleFile(file))
        {
            insertFromDescription(drag::sample(file), index);
            break; // one Sampler per chain
        }
    }
}

} // namespace lattice::debugui
