#include "ui/device/DeviceView.h"

#include "devices/ChainEditing.h"
#include "devices/racks/Rack.h"
#include "devices/racks/RackEditing.h"
#include "ui/device/DevicePanels.h"
#include "ui/session/SessionView.h"
#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui
{
namespace
{
constexpr auto samplerType = "lattice.sampler";
constexpr double flashSeconds = 1.2;

/** Every frame inside a component, depth first. */
void forEachFrame(juce::Component& root, const std::function<void(DeviceFrame&)>& fn)
{
    for (auto* child : root.getChildren())
    {
        if (auto* frame = dynamic_cast<DeviceFrame*>(child))
            fn(*frame);
        forEachFrame(*child, fn);
    }
}

/** The device type a description would insert ("" for a move or something unreadable). */
juce::String typeOfDescription(const DeviceContext& context, const juce::String& description, presets::Preset* preset)
{
    const auto payload = drag::payload(description);
    if (drag::is(description, "device"))
        return payload;
    if (drag::is(description, "sample"))
        return samplerType;
    if (drag::is(description, "preset"))
    {
        const int i = payload.getIntValue();
        const auto& all = context.presets.all();
        presets::Preset p;
        if (i < 0 || i >= static_cast<int>(all.size()) || context.presets.load(all[static_cast<std::size_t>(i)], p).failed())
            return {};
        const auto type = p.device[ids::deviceType].toString();
        if (preset != nullptr)
            *preset = std::move(p);
        return type;
    }
    if (drag::is(description, "move"))
        return context.set().deviceById(payload.getLargeIntValue())[ids::deviceType].toString();
    return {};
}
} // namespace

DeviceView::DeviceView(DeviceContext& c, SessionSelectionModel& s) : context(c), selection(s)
{
    context.view = this;
    setTitle("Device view");
    setWantsKeyboardFocus(true);
    scroller.setScrollBarsShown(false, true);
    scroller.setWantsKeyboardFocus(false);
    addAndMakeVisible(scroller);
    shown = selection.deviceOwner();
    selection.addListener(this);
    context.set().state().addListener(this);
    rebuild();
}

DeviceView::~DeviceView()
{
    cancelPendingUpdate();
    context.set().state().removeListener(this);
    selection.removeListener(this);
    scroller.setViewedComponent(nullptr, false);
    top.reset();
    if (context.view == this)
        context.view = nullptr;
}

// --- What it shows ------------------------------------------------------------------------------------

juce::ValueTree DeviceView::ownerNode() const
{
    auto& set = context.set();
    switch (shown.kind)
    {
    case DeviceOwner::Kind::track:
        return shown.index >= 0 && shown.index < set.numTracks() ? set.track(shown.index) : juce::ValueTree();
    case DeviceOwner::Kind::returnTrack:
        return shown.index >= 0 && shown.index < set.numReturns() ? set.returnTrack(shown.index) : juce::ValueTree();
    case DeviceOwner::Kind::master:
        return set.master();
    }
    return {};
}

juce::String DeviceView::ownerName() const
{
    const auto owner = ownerNode();
    if (!owner.isValid())
        return {};
    return shown.kind == DeviceOwner::Kind::master ? juce::String("Master") : owner[ids::name].toString();
}

void DeviceView::deviceOwnerChanged(const DeviceOwner& now)
{
    if (now == shown)
        return;
    shown = now;
    rebuild();
}

void DeviceView::rebuild()
{
    needsRebuild = false;
    const auto chain = LiveSet::chainOf(ownerNode());
    if (chain != shownChain || (top == nullptr && chain.isValid()))
    {
        shownChain = chain;
        scroller.setViewedComponent(nullptr, false);
        top.reset();
        if (chain.isValid())
        {
            top = std::make_unique<ChainView>(context, chain);
            scroller.setViewedComponent(top.get(), false);
        }
        scroller.setViewPosition(0, 0);
    }
    // Forget what's no longer in the document.
    const auto root = context.set().state();
    for (auto* v : {&selected, &focused, &swapTarget, &mapRack})
        if (v->isValid() && !v->isAChildOf(root))
            *v = {};
    if (!mapRack.isValid())
        mapMacro = -1;
    titleMaybeChanged();
    layout();
}

DeviceFrame* DeviceView::frameFor(const juce::ValueTree& device) const
{
    DeviceFrame* found = nullptr;
    if (top != nullptr)
        forEachFrame(*top, [&](DeviceFrame& f) {
            if (found == nullptr && f.device() == device)
                found = &f;
        });
    return found;
}

void DeviceView::titleMaybeChanged()
{
    const auto name = ownerName();
    if (name != lastTitle)
    {
        lastTitle = name;
        if (onTitleChanged)
            onTitleChanged(name);
    }
}

// --- Layout ---------------------------------------------------------------------------------------------

void DeviceView::requestLayout()
{
    needsLayout = true;
    triggerAsyncUpdate();
}

void DeviceView::handleAsyncUpdate()
{
    if (needsRebuild)
        rebuild();
    if (needsLayout)
        layout();
}

void DeviceView::flushUpdates()
{
    for (int pass = 0; pass < 4; ++pass) // nested chains may ask again once they're built
    {
        handleUpdateNowIfNeeded();
        if (top != nullptr)
            top->flushUpdates();
        if (!isUpdatePending())
            break;
    }
    handleUpdateNowIfNeeded();
}

void DeviceView::layout()
{
    needsLayout = false;
    if (top == nullptr)
        return;
    const int height = juce::jmax(0, scroller.getHeight() - scroller.getScrollBarThickness());
    top->setSize(juce::jmax(top->preferredWidth(height), scroller.getWidth()), height);
    top->resized();
}

void DeviceView::resized()
{
    scroller.setBounds(getLocalBounds());
    layout();
}

void DeviceView::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    g.fillAll(t.surface);
    if (top == nullptr)
    {
        g.setColour(t.textDim);
        g.setFont(t.font(t.fontNormal));
        g.drawFittedText("Select a track to see its devices.", getLocalBounds().reduced(juce::roundToInt(t.padding * 2.0f)),
                         juce::Justification::centred, 2);
    }
}

void DeviceView::paintOverChildren(juce::Graphics& g)
{
    // A control just found (revealParameter) flashes for a moment.
    if (auto* c = flashed.getComponent(); c != nullptr && c->isShowing() &&
                                          juce::Time::getMillisecondCounterHiRes() * 0.001 < flashUntil)
    {
        const auto& t = themeOf(*this);
        g.setColour(t.focus);
        g.drawRoundedRectangle(getLocalArea(c, c->getLocalBounds()).toFloat().expanded(2.0f), t.radius, juce::jmax(2.0f, t.stroke * 2.0f));
    }
}

void DeviceView::scrollIntoView(juce::Component& c)
{
    if (top == nullptr || !top->isParentOf(&c))
        return;
    const auto r = top->getLocalArea(&c, c.getLocalBounds());
    auto pos = scroller.getViewPosition();
    if (r.getX() < pos.x)
        pos.x = r.getX();
    else if (r.getRight() > pos.x + scroller.getViewWidth())
        pos.x = r.getRight() - scroller.getViewWidth();
    scroller.setViewPosition(pos);
}

// --- Selection ------------------------------------------------------------------------------------------

void DeviceView::select(const juce::ValueTree& device)
{
    if (device != selected)
    {
        selected = device;
        if (top != nullptr)
            top->repaint();
    }
    if (device.isValid())
        focused = device.getParent();
    if (isShowing())
        grabKeyboardFocus();
}

void DeviceView::focusChain(const juce::ValueTree& devicesNode)
{
    if (devicesNode != focused)
    {
        focused = devicesNode;
        // The selection moves to the chain clicked (if it isn't already in it).
        if (selected.isValid() && selected.getParent() != focused)
            selected = {};
        if (top != nullptr)
            top->repaint();
    }
    if (isShowing())
        grabKeyboardFocus();
}

void DeviceView::moveSelection(int delta)
{
    auto chain = selected.isValid() ? selected.getParent() : insertionChain();
    if (!chain.isValid() || chain.getNumChildren() == 0)
        return;
    const int at = selected.isValid() ? chain.indexOf(selected) + delta : (delta > 0 ? 0 : chain.getNumChildren() - 1);
    select(chain.getChild(juce::jlimit(0, chain.getNumChildren() - 1, at)));
    if (auto* f = frameFor(selected))
        scrollIntoView(*f);
}

bool DeviceView::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::leftKey || key == juce::KeyPress::rightKey)
    {
        moveSelection(key == juce::KeyPress::leftKey ? -1 : 1);
        return true;
    }
    if (key == juce::KeyPress::escapeKey && (swapTarget.isValid() || mapRack.isValid()))
    {
        setHotSwapTarget({});
        setMapMode({}, -1);
        return true;
    }
    return false;
}

void DeviceView::mouseDown(const juce::MouseEvent&)
{
    if (top != nullptr)
        focusChain(top->chain());
}

// --- Inserting -------------------------------------------------------------------------------------------

juce::ValueTree DeviceView::insertionChain() const
{
    const auto root = context.set().state();
    if (selected.isValid() && selected.isAChildOf(root) && selected.getParent().isAChildOf(shownChain.getParent()))
        return selected.getParent();
    if (focused.isValid() && focused.isAChildOf(root) && (focused == shownChain || focused.isAChildOf(shownChain)))
        return focused;
    return shownChain;
}

int DeviceView::insertionIndex() const
{
    const auto chain = insertionChain();
    if (selected.isValid() && selected.getParent() == chain)
        return chain.indexOf(selected) + 1;
    return -1;
}

bool DeviceView::insert(const juce::String& description, const juce::ValueTree& chain, int index)
{
    if (!chain.isValid())
        return false;
    if (swapTarget.isValid() && !drag::is(description, "move"))
        return replaceDevice(swapTarget, description);

    const auto type = typeOfDescription(context, description, nullptr);
    if (type.isEmpty())
        return false;
    const auto moving = drag::is(description, "move") ? context.set().deviceById(drag::payload(description).getLargeIntValue())
                                                      : juce::ValueTree();
    const auto placement = chains::place(chain, type, index, context.registry(), moving);
    if (!placement.allowed)
    {
        if (context.confirm)
            context.confirm("Can't put that here", placement.reason, {}, nullptr);
        return false;
    }
    if (placement.replaces.isValid() && placement.replaces != moving)
    {
        // Never replace an instrument silently (M3 hands-on finding 4).
        const auto question = "This chain already has " + context.nameOf(placement.replaces) + ". Replace it with " +
                              (drag::is(description, "move") ? context.nameOf(moving) : context.registry().find(type) != nullptr
                                                                                             ? context.registry().find(type)->name
                                                                                             : type) +
                              "? (Hot-swap replaces a device without asking.)";
        juce::Component::SafePointer<DeviceView> safe(this);
        if (context.confirm)
            context.confirm("Replace the instrument?", question, "Replace", [safe, description, chain, index] {
                if (safe != nullptr)
                    safe->insertNow(description, chain, index);
            });
        return false;
    }
    return insertNow(description, chain, index);
}

bool DeviceView::insertNow(const juce::String& description, const juce::ValueTree& chain, int index)
{
    auto& set = context.set();
    const auto& registry = context.registry();
    const auto payload = drag::payload(description);
    juce::ValueTree added;
    if (drag::is(description, "device"))
    {
        context.undo().beginNewTransaction("Insert device");
        added = chains::insert(set, chain, payload, index, registry);
    }
    else if (drag::is(description, "preset"))
    {
        presets::Preset p;
        if (typeOfDescription(context, description, &p).isEmpty())
            return false;
        context.undo().beginNewTransaction("Load preset");
        added = chains::insertCopy(set, chain, p.device, index, registry);
    }
    else if (drag::is(description, "sample"))
    {
        const juce::File file(payload);
        if (!isSampleFile(file))
            return false;
        context.undo().beginNewTransaction("Load sample");
        added = chains::insert(set, chain, samplerType, index, registry);
        if (added.isValid())
            panels::loadSampleInto(context, added, file, false);
    }
    else if (drag::is(description, "move"))
    {
        const auto device = set.deviceById(payload.getLargeIntValue());
        if (!device.isValid())
            return false;
        context.undo().beginNewTransaction("Move device");
        if (chains::move(set, device, chain, index, registry))
            added = device;
    }
    if (!added.isValid())
        return false;
    flushUpdates();
    select(added);
    if (auto* f = frameFor(added))
        scrollIntoView(*f);
    return true;
}

bool DeviceView::replaceDevice(const juce::ValueTree& target, const juce::String& description)
{
    auto chain = target.getParent();
    if (!chain.isValid())
        return false;
    const int index = chain.indexOf(target);
    auto& set = context.set();
    const auto& registry = context.registry();
    presets::Preset preset;
    const auto type = typeOfDescription(context, description, &preset);
    if (type.isEmpty())
        return false;
    context.undo().beginNewTransaction("Hot-swap");
    set.removeDevice(target);
    juce::ValueTree added;
    if (drag::is(description, "preset"))
        added = chains::insertCopy(set, chain, preset.device, index, registry);
    else
    {
        added = chains::insert(set, chain, type, index, registry);
        if (added.isValid() && drag::is(description, "sample"))
            panels::loadSampleInto(context, added, juce::File(drag::payload(description)), false);
    }
    setHotSwapTarget({});
    if (!added.isValid())
    {
        context.undo().undoCurrentTransactionOnly(); // it couldn't go there: put the old one back
        return false;
    }
    flushUpdates();
    select(added);
    return true;
}

juce::PopupMenu DeviceView::insertMenu(const juce::ValueTree& chain, int index)
{
    juce::PopupMenu m;
    juce::Component::SafePointer<DeviceView> safe(this);
    const auto inserting = [safe, chain, index](const juce::String& description) {
        return [safe, chain, index, description] {
            if (safe != nullptr)
                safe->insert(description, chain, index);
        };
    };
    if (swapTarget.isValid())
        m.addSectionHeader("Swap " + context.nameOf(swapTarget) + " for");
    std::map<juce::String, juce::PopupMenu> byCategory;
    juce::StringArray order;
    for (const auto& type : context.registry().types())
    {
        if (type.hidden)
            continue;
        if (!order.contains(type.category))
            order.add(type.category);
        const bool fits = chains::place(chain, type.id, index, context.registry()).allowed;
        byCategory[type.category].addItem(type.name, fits, false, inserting(drag::device(type.id)));
    }
    for (const auto& category : order)
        m.addSubMenu(category, byCategory[category]);
    const auto& all = context.presets.all();
    if (!all.empty())
    {
        juce::PopupMenu presetMenu;
        std::map<juce::String, juce::PopupMenu> byDevice;
        juce::StringArray devicesOrder;
        for (std::size_t i = 0; i < all.size(); ++i)
        {
            const auto* info = context.registry().find(all[i].deviceType);
            const auto name = info != nullptr ? info->name : all[i].deviceType;
            if (!devicesOrder.contains(name))
                devicesOrder.add(name);
            byDevice[name].addItem(all[i].name + (all[i].factory ? "" : "  (user)"), inserting(drag::preset(static_cast<int>(i))));
        }
        for (const auto& name : devicesOrder)
            presetMenu.addSubMenu(name, byDevice[name]);
        m.addSubMenu("Presets", presetMenu);
    }
    m.addSeparator();
    m.addItem("Load a Sample...", [safe, chain, index] {
        if (safe == nullptr || !safe->context.chooseFile)
            return;
        safe->context.chooseFile("Load a sample into a new Sampler", sampleFilePatterns, [safe, chain, index](const juce::File& f) {
            if (safe != nullptr)
                safe->insert(drag::sample(f), chain, index);
        });
    });
    return m;
}

// --- Hot-swap and map mode ---------------------------------------------------------------------------------

void DeviceView::setHotSwapTarget(const juce::ValueTree& device)
{
    if (device == swapTarget)
        return;
    swapTarget = device;
    if (device.isValid())
        select(device);
    if (top != nullptr)
        top->repaint();
}

juce::ValueTree DeviceView::selectedChainOf(const juce::ValueTree& rack) const
{
    const auto chains = rack.getChildWithName(ids::rackChains);
    if (const auto it = chainChoice.find(LiveSet::idOf(rack)); it != chainChoice.end())
        for (const auto& c : chains)
            if (LiveSet::idOf(c) == it->second)
                return c;
    return chains.getChild(0);
}

void DeviceView::selectChain(const juce::ValueTree& rack, const juce::ValueTree& chain)
{
    chainChoice[LiveSet::idOf(rack)] = LiveSet::idOf(chain);
    if (chain.isValid())
        focusChain(racks::devicesOf(chain));
    if (auto* f = frameFor(rack); f != nullptr && f->body() != nullptr)
        f->body()->viewStateChanged();
    requestLayout();
}

void DeviceView::setMapMode(const juce::ValueTree& rack, int macro)
{
    mapRack = rack;
    mapMacro = rack.isValid() ? macro : -1;
    if (top != nullptr)
        forEachFrame(*top, [](DeviceFrame& f) {
            f.mapModeChanged();
            if (f.body() != nullptr)
                f.body()->viewStateChanged();
        });
}

bool DeviceView::mapTo(const juce::ValueTree& device, const juce::String& parameterId)
{
    if (!mapRack.isValid() || mapMacro < 0 || !device.isAChildOf(mapRack))
        return false;
    // Mapping a parameter the macro already has unmaps it (a second click takes it back).
    for (const auto& m : racks::mappingsOf(mapRack, mapMacro))
        if (static_cast<juce::int64>(m[ids::targetDevice]) == LiveSet::idOf(device) && m[ids::parameter].toString() == parameterId)
        {
            context.undo().beginNewTransaction("Remove mapping");
            racks::removeMapping(context.set(), m);
            return true;
        }
    context.undo().beginNewTransaction("Map to macro");
    racks::addMapping(context.set(), mapRack, mapMacro, device, parameterId);
    return true;
}

// --- Finding controls ---------------------------------------------------------------------------------------

juce::Component* DeviceView::revealParameter(const juce::ValueTree& device, const juce::String& parameterId)
{
    auto& set = context.set();
    if (!device.isValid() || !device.isAChildOf(set.state()))
        return nullptr;
    const auto owner = LiveSet::chainOwnerOf(device);
    if (owner.hasType(ids::track))
    {
        const int index = set.trackIndexForId(LiveSet::idOf(owner));
        selection.set({SessionSelection::Kind::track, index, selection.get().scene});
    }
    else if (owner.hasType(ids::returnTrack))
        for (int r = 0; r < set.numReturns(); ++r)
        {
            if (set.returnTrack(r) == owner)
                selection.showDevicesOf({DeviceOwner::Kind::returnTrack, r});
        }
    else if (owner.hasType(ids::master))
        selection.showDevicesOf({DeviceOwner::Kind::master, 0});
    if (onShowRequested)
        onShowRequested();
    flushUpdates();

    // Open the rack chains it's in, outermost first, unfolding the racks.
    std::vector<std::pair<juce::ValueTree, juce::ValueTree>> path; // rack, chain
    for (auto n = device.getParent(); n.isValid(); n = n.getParent())
        if (n.hasType(ids::rackChain))
            path.insert(path.begin(), {n.getParent().getParent(), n});
    for (const auto& [rack, chain] : path)
    {
        LiveSet::setDeviceFolded(rack, false);
        selectChain(rack, chain);
        flushUpdates();
    }
    auto* frame = frameFor(device);
    if (frame == nullptr)
        return nullptr;
    auto* control = frame->revealParameter(parameterId);
    flushUpdates();
    layout();
    if (control == nullptr)
        return nullptr;
    select(device);
    scrollIntoView(*control);
    flashed = control;
    flashUntil = juce::Time::getMillisecondCounterHiRes() * 0.001 + flashSeconds;
    repaint();
    juce::Component::SafePointer<DeviceView> safe(this);
    juce::Timer::callAfterDelay(juce::roundToInt(flashSeconds * 1000.0) + 50, [safe] {
        if (safe != nullptr)
            safe->repaint();
    });
    return control;
}

// --- Edit commands -----------------------------------------------------------------------------------------

EditTarget* DeviceView::editTargetIfFocused()
{
    return hasKeyboardFocus(true) ? this : nullptr;
}

bool DeviceView::canEdit(const juce::String& id) const
{
    if (id == "edit.paste")
        return clipboard.isValid() && insertionChain().isValid();
    if (id == "edit.duplicate" || id == "edit.delete" || id == "edit.copy" || id == "edit.cut" || id == "edit.rename")
        return selected.isValid() && selected.isAChildOf(context.set().state());
    return false;
}

void DeviceView::copySelected()
{
    if (selected.isValid())
        clipboard = selected.createCopy();
}

void DeviceView::pasteHere()
{
    auto chain = insertionChain();
    if (!clipboard.isValid() || !chain.isValid())
        return;
    context.undo().beginNewTransaction("Paste device");
    const auto added = chains::insertCopy(context.set(), chain, clipboard, insertionIndex(), context.registry());
    if (added.isValid())
    {
        flushUpdates();
        select(added);
    }
}

void DeviceView::renameSelected()
{
    const auto device = selected;
    askForText(*this, "Rename Device", context.nameOf(device), [this, device](const juce::String& name) {
        context.undo().beginNewTransaction("Rename device");
        context.set().setDeviceName(device, name);
    });
}

void DeviceView::edit(const juce::String& id)
{
    if (!canEdit(id))
        return;
    if (id == "edit.copy")
        copySelected();
    else if (id == "edit.paste")
        pasteHere();
    else if (id == "edit.rename")
        renameSelected();
    else if (id == "edit.duplicate")
    {
        auto chain = selected.getParent();
        context.undo().beginNewTransaction("Duplicate device");
        const auto copy = chains::insertCopy(context.set(), chain, selected, chain.indexOf(selected) + 1, context.registry());
        if (copy.isValid())
        {
            flushUpdates();
            select(copy);
        }
    }
    else if (id == "edit.delete" || id == "edit.cut")
    {
        if (id == "edit.cut")
            copySelected();
        auto chain = selected.getParent();
        const int index = chain.indexOf(selected);
        context.undo().beginNewTransaction(id == "edit.cut" ? "Cut device" : "Delete device");
        context.set().removeDevice(selected);
        selected = {};
        // The next one along takes the selection, as in Live.
        if (chain.getNumChildren() > 0)
            selected = chain.getChild(juce::jmin(index, chain.getNumChildren() - 1));
        focused = chain;
        flushUpdates();
    }
}

// --- The document -------------------------------------------------------------------------------------------

void DeviceView::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree&)
{
    // Tracks, returns or the chain's own node came or went: whatever's shown may have moved.
    if (parent.hasType(ids::tracks) || parent.hasType(ids::returns) || parent == ownerNode())
    {
        needsRebuild = true;
        triggerAsyncUpdate();
    }
}

void DeviceView::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int)
{
    needsRebuild = needsRebuild || parent.hasType(ids::tracks) || parent.hasType(ids::returns) || parent == ownerNode() ||
                   (selected.isValid() && !selected.isAChildOf(context.set().state()));
    if (needsRebuild)
        triggerAsyncUpdate();
}

void DeviceView::valueTreeChildOrderChanged(juce::ValueTree& parent, int, int)
{
    if (parent.hasType(ids::tracks) || parent.hasType(ids::returns))
    {
        needsRebuild = true;
        triggerAsyncUpdate();
    }
}

void DeviceView::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    if (property == ids::name && tree == ownerNode())
        titleMaybeChanged();
}

void DeviceView::valueTreeRedirected(juce::ValueTree&)
{
    needsRebuild = true;
    triggerAsyncUpdate();
}

} // namespace lattice::ui
