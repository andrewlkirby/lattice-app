#include "control/modes/DeviceMode.h"

#include "control/modes/PadFaders.h"
#include "core/model/UndoNames.h"
#include "devices/framework/ParameterPager.h"
#include "devices/racks/Rack.h"
#include "devices/racks/RackEditing.h"
#include "ui/mapping/Targets.h"

#include <algorithm>

namespace lattice::control
{

namespace
{
const juce::String back = juce::String::fromUTF8("\xe2\x80\xb9 "); // "< "
const juce::String crumb = juce::String::fromUTF8(" \xe2\x80\xba ");  // " > "
const juce::String dash = juce::String::fromUTF8(" \xe2\x80\x93 ");  // " - "

bool isRack(const juce::ValueTree& device)
{
    return device.isValid() && RackDevice::isRackType(device[ids::deviceType].toString());
}

/** The rack a chain's DEVICES node is in, or an invalid tree (a top-level chain). */
juce::ValueTree rackAbove(const juce::ValueTree& devices)
{
    const auto chain = devices.getParent();
    return chain.hasType(ids::rackChain) ? chain.getParent().getParent() : juce::ValueTree();
}
} // namespace

DeviceMode::DeviceMode(ModeHost& h) : Mode(h) {}

// --- What it shows ------------------------------------------------------------------------------------

juce::ValueTree DeviceMode::owner() const
{
    auto& c = host.context();
    const auto& set = c.session.set();
    const auto& o = c.selection.deviceOwner();
    switch (o.kind)
    {
    case ui::DeviceOwner::Kind::track: return o.index >= 0 && o.index < set.numTracks() ? set.track(o.index) : juce::ValueTree();
    case ui::DeviceOwner::Kind::returnTrack: return o.index >= 0 && o.index < set.numReturns() ? set.returnTrack(o.index) : juce::ValueTree();
    case ui::DeviceOwner::Kind::master: return set.master();
    }
    return {};
}

juce::ValueTree DeviceMode::device() const
{
    const auto own = owner();
    if (!own.isValid())
        return {};
    if (emptyChain.isValid() && emptyChain.getNumChildren() == 0 && emptyChain.isAChildOf(own))
        return {};
    juce::ValueTree d;
    if (host.followsSelection())
        d = ui::findDeviceById(own, host.context().selection.selectedDevice());
    else if (local.isValid() && local.isAChildOf(own))
        d = local;
    return d.isValid() ? d : LiveSet::chainOf(own).getChild(0);
}

juce::ValueTree DeviceMode::level() const
{
    const auto own = owner();
    if (emptyChain.isValid() && emptyChain.getNumChildren() == 0 && own.isValid() && emptyChain.isAChildOf(own))
        return emptyChain;
    const auto d = device();
    return d.isValid() ? d.getParent() : LiveSet::chainOf(own);
}

void DeviceMode::selectDevice(const juce::ValueTree& d)
{
    local = d;
    chainsOf = {};
    emptyChain = {};
    if (host.followsSelection())
        host.context().selection.selectDevice(LiveSet::idOf(d));
    host.requestRefresh();
}

juce::String DeviceMode::nameOf(const juce::ValueTree& d) const
{
    return undoNames::deviceName(d, host.context().session.deviceRegistry());
}

int DeviceMode::controls() const
{
    return caps().encoders > 0 ? caps().encoders : caps().columns;
}

std::vector<ParameterGroup> DeviceMode::pages() const
{
    const auto d = device();
    if (!d.isValid())
        return {};
    const auto type = d[ids::deviceType].toString();
    if (RackDevice::isRackType(type))
        return RackDevice::macroPages(racks::macroCount(d), controls());
    if (const auto* spec = host.specs().forType(type))
        return ParameterPager::pagesFor(*spec, controls());
    return {};
}

int DeviceMode::page() const
{
    const auto d = device();
    const int count = static_cast<int>(pages().size());
    const auto it = pageOf.find(LiveSet::idOf(d));
    return it == pageOf.end() || count == 0 ? 0 : juce::jlimit(0, count - 1, it->second);
}

void DeviceMode::setPage(int p)
{
    const auto d = device();
    const int count = static_cast<int>(pages().size());
    if (!d.isValid() || count == 0)
        return;
    pageOf[LiveSet::idOf(d)] = juce::jlimit(0, count - 1, p);
    host.requestRefresh();
}

juce::String DeviceMode::parameterId(const juce::ValueTree& d, int index) const
{
    const auto* spec = host.specs().forType(d[ids::deviceType].toString());
    return spec != nullptr && index >= 0 && index < spec->numParameters() ? spec->parameter(index).spec().id : juce::String();
}

std::vector<DeviceMode::Item> DeviceMode::items() const
{
    std::vector<Item> out;
    const auto& state = host.context().session.set().state();
    if (chainsOf.isValid() && chainsOf.isAChildOf(state))
    {
        out.push_back({Item::Kind::back, chainsOf, back + nameOf(chainsOf), false});
        const auto chains = chainsOf.getChildWithName(ids::rackChains);
        for (int i = 0; i < chains.getNumChildren(); ++i)
        {
            const auto chain = chains.getChild(i);
            const auto name = chain[ids::name].toString();
            out.push_back({Item::Kind::chain, chain, name.isNotEmpty() ? name : "Chain " + juce::String(i + 1), false});
        }
        return out;
    }
    const auto lvl = level();
    if (const auto rack = rackAbove(lvl); rack.isValid())
        out.push_back({Item::Kind::back, rack, back + nameOf(rack), false});
    const auto d = device();
    for (const auto& child : lvl)
        out.push_back({Item::Kind::device, child, nameOf(child), child == d});
    return out;
}

int DeviceMode::firstItem() const
{
    const int n = caps().countOf(Role::displayUpper);
    if (n <= 0)
        return 0;
    const auto all = items();
    const auto it = std::find_if(all.begin(), all.end(), [](const Item& i) { return i.selected; });
    const int selected = it == all.end() ? 0 : static_cast<int>(it - all.begin());
    return selected / n * n;
}

void DeviceMode::enterRack()
{
    if (const auto d = device(); isRack(d))
    {
        chainsOf = d;
        emptyChain = {};
        host.requestRefresh();
    }
}

void DeviceMode::leaveRack()
{
    if (chainsOf.isValid())
    {
        chainsOf = {};
        host.requestRefresh();
        return;
    }
    if (const auto rack = rackAbove(level()); rack.isValid())
        selectDevice(rack);
}

void DeviceMode::pressItem(int index)
{
    const auto all = items();
    if (index < 0 || index >= static_cast<int>(all.size()))
        return;
    const auto& item = all[static_cast<std::size_t>(index)];
    switch (item.kind)
    {
    case Item::Kind::back: leaveRack(); return;
    case Item::Kind::chain:
    {
        const auto devices = racks::devicesOf(item.node);
        if (devices.getNumChildren() > 0)
            selectDevice(devices.getChild(0));
        else
        {
            chainsOf = {};
            emptyChain = devices;
            host.requestRefresh();
        }
        return;
    }
    case Item::Kind::device:
        if (item.selected && isRack(item.node))
            enterRack();
        else
            selectDevice(item.node);
        return;
    }
}

int DeviceMode::pageButtons() const
{
    return std::max(0, caps().countOf(Role::displayLower) - 1);
}

int DeviceMode::firstPage() const
{
    const int n = pageButtons();
    return n > 0 ? page() / n * n : 0;
}

void DeviceMode::pressLower(int index)
{
    const auto d = device();
    if (!d.isValid())
        return;
    if (index == 0)
    {
        auto& c = host.context();
        const bool on = d.getProperty(ids::enabled, true);
        c.session.beginEdit(on ? "Device off" : "Device on", d);
        c.session.set().setDeviceEnabled(d, !on);
        return;
    }
    const int p = firstPage() + index - 1;
    if (p < static_cast<int>(pages().size()))
        setPage(p);
}

void DeviceMode::step(int direction)
{
    const auto d = device();
    const auto lvl = level();
    if (!d.isValid() || !lvl.isValid())
        return;
    const int at = lvl.indexOf(d) + direction;
    if (at >= 0 && at < lvl.getNumChildren())
        selectDevice(lvl.getChild(at));
}

// --- Input ----------------------------------------------------------------------------------------------

bool DeviceMode::handle(const SurfaceInput& input)
{
    using Kind = SurfaceInput::Kind;
    if (input.kind == Kind::buttonDown)
    {
        const auto id = input.button;
        switch (id.role)
        {
        case Role::displayUpper: pressItem(firstItem() + id.index); return true;
        case Role::displayLower: pressLower(id.index); return true;
        default: break;
        }
        if (!padsOwned())
            return false;
        // Without encoders: the grid is the faders, so the arrows and scene buttons are this mode's.
        switch (id.role)
        {
        case Role::scene:
        {
            const int n = caps().countOf(Role::scene);
            setPage(page() / std::max(1, n) * n + id.index);
            return true;
        }
        case Role::navLeft: step(-1); return true;
        case Role::navRight: step(1); return true;
        case Role::navUp: leaveRack(); return true;
        case Role::navDown:
            if (const auto d = device(); isRack(d))
            {
                enterRack();
                pressItem(1); // its first chain
            }
            return true;
        default: return false;
        }
    }
    if (input.kind == Kind::buttonUp)
    {
        const auto role = input.button.role;
        return role == Role::displayUpper || role == Role::displayLower ||
               (padsOwned() && (role == Role::scene || role == Role::navLeft || role == Role::navRight || role == Role::navUp || role == Role::navDown));
    }
    if (!padsOwned())
        return false;
    if (input.kind == Kind::padDown)
    {
        if (auto* binding = host.encoders().binding(input.column))
            binding->setNormalised(faders::positionOfRow(input.row, caps().rows)); // a step of its own
        return true;
    }
    return input.kind == Kind::padUp || input.kind == Kind::padPressure;
}

// --- Output ---------------------------------------------------------------------------------------------

void DeviceMode::encoderTargets(std::vector<juce::String>& targets)
{
    const auto d = device();
    const auto all = pages();
    if (!d.isValid() || all.empty())
        return;
    for (const int index : all[static_cast<std::size_t>(page())].parameters)
        targets.push_back(ui::deviceTarget(d, parameterId(d, index)));
}

void DeviceMode::display(DisplayFrame& f)
{
    const auto own = owner();
    const auto d = device();
    const auto ownName = own.hasType(ids::master) ? juce::String("Master") : own[ids::name].toString();
    f.title = d.isValid() ? ownName + crumb + nameOf(d) : ownName;

    const auto all = items();
    const int first = firstItem();
    for (int i = 0; i < caps().countOf(Role::displayUpper); ++i)
    {
        const int at = first + i;
        if (at >= static_cast<int>(all.size()))
            break;
        const auto& item = all[static_cast<std::size_t>(at)];
        f.upper.push_back({item.label, item.selected ? rgbOf(static_cast<std::uint32_t>(static_cast<juce::int64>(own[ids::colour]))) : 0u,
                           item.selected});
    }

    if (!d.isValid())
    {
        f.message = level() == emptyChain && emptyChain.isValid() ? "Empty chain" : "No devices";
        return;
    }
    auto& bank = host.encoders();
    const bool rack = isRack(d);
    const auto params = pages();
    if (params.empty())
    {
        f.message = "No parameters";
        return;
    }
    const auto& shown = params[static_cast<std::size_t>(page())];
    for (int i = 0; i < controls(); ++i)
    {
        DisplayCell cell;
        if (auto* b = i < static_cast<int>(shown.parameters.size()) ? bank.binding(i) : nullptr)
        {
            const auto& spec = b->spec();
            const int index = shown.parameters[static_cast<std::size_t>(i)];
            cell.name = rack ? racks::macroName(d, index) : spec.name;
            cell.value = b->text();
            cell.position = b->normalised();
            cell.bipolar = spec.kind == ParamKind::Float && spec.minValue < 0.0f && spec.maxValue > 0.0f;
            cell.colour = rack ? rgbOf(racks::macroColour(d, index)) : 0u;
            cell.detail = b->textFor(spec.minValue) + dash + b->textFor(spec.maxValue);
        }
        f.cells.push_back(std::move(cell));
    }
    f.detail = bank.touchedEncoder();

    if (caps().countOf(Role::displayLower) > 0)
    {
        const bool on = d.getProperty(ids::enabled, true);
        f.lower.push_back({on ? "On" : "Off", 0u, on});
        for (int i = 0; i < pageButtons(); ++i)
        {
            const int p = firstPage() + i;
            if (p >= static_cast<int>(params.size()))
                break;
            f.lower.push_back({params[static_cast<std::size_t>(p)].name, 0u, p == page()});
        }
    }
}

void DeviceMode::describe(SurfaceText& text)
{
    const auto d = device();
    if (!d.isValid())
    {
        text.status = "Device: no devices";
        return;
    }
    const auto all = pages();
    text.status = "Device: " + nameOf(d);
    if (!all.empty())
        text.status << " \xc2\xb7 " << all[static_cast<std::size_t>(page())].name << " (" << (page() + 1) << "/"
                    << static_cast<int>(all.size()) << ")";
}

void DeviceMode::render(LedFrame& frame)
{
    const auto& colours = host.colours();
    const auto available = LedState::solid(dimmed(colours.white, colours.dim));
    const auto lit = LedState::solid(colours.white);
    const auto all = items();
    const int first = firstItem();
    for (int i = 0; i < caps().countOf(Role::displayUpper); ++i)
    {
        const int at = first + i;
        setButton(frame, {Role::displayUpper, i},
                  at < static_cast<int>(all.size()) ? (all[static_cast<std::size_t>(at)].selected ? lit : available) : LedState::off());
    }
    const auto d = device();
    const int numPages = static_cast<int>(pages().size());
    const int lower = caps().countOf(Role::displayLower);
    for (int i = 0; i < lower; ++i)
    {
        LedState led = LedState::off();
        if (d.isValid() && i == 0)
            led = static_cast<bool>(d.getProperty(ids::enabled, true)) ? lit : available;
        else if (d.isValid() && firstPage() + i - 1 < numPages)
            led = firstPage() + i - 1 == page() ? lit : available;
        setButton(frame, {Role::displayLower, i}, led);
    }
    if (!padsOwned())
        return;

    // The grid's columns as faders, in the owner's colour.
    const auto own = owner();
    const auto colour = own.hasProperty(ids::colour) ? rgbOf(static_cast<std::uint32_t>(static_cast<juce::int64>(own[ids::colour]))) : colours.white;
    for (int column = 0; column < caps().columns; ++column)
        if (const auto* b = host.encoders().binding(column))
            faders::renderColumn(frame, caps(), column, b->normalised(),
                                 b->spec().kind == ParamKind::Float && b->spec().minValue < 0.0f && b->spec().maxValue > 0.0f, colour, 0.12f);
    const int scenes = caps().countOf(Role::scene);
    const int firstScenePage = scenes > 0 ? page() / scenes * scenes : 0;
    for (int i = 0; i < scenes; ++i)
        setButton(frame, {Role::scene, i}, firstScenePage + i >= numPages ? LedState::off() : firstScenePage + i == page() ? lit : available);
    const auto lvl = level();
    const int at = d.isValid() ? lvl.indexOf(d) : -1;
    setButton(frame, {Role::navLeft, 0}, at > 0 ? available : LedState::off());
    setButton(frame, {Role::navRight, 0}, at >= 0 && at + 1 < lvl.getNumChildren() ? available : LedState::off());
    setButton(frame, {Role::navUp, 0}, rackAbove(lvl).isValid() || chainsOf.isValid() ? available : LedState::off());
    setButton(frame, {Role::navDown, 0}, isRack(d) ? available : LedState::off());
}

} // namespace lattice::control
