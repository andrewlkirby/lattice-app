#include "control/modes/ClipMode.h"

#include "control/modes/PadFaders.h"
#include "control/modes/SelectedClip.h"
#include "ui/clip/ClipActions.h"
#include "ui/clip/ClipBindings.h"

#include <algorithm>

namespace lattice::control
{

namespace
{
const juce::String dot = juce::String::fromUTF8(" \xc2\xb7 ");

/** The values, in groups that stay together on a page when they fit. */
struct Group
{
    const char* name;
    std::vector<juce::String> values;
};
const std::vector<Group>& groups()
{
    static const std::vector<Group> list{
        {"Loop", {"looping", "startMarker", "loopStart", "loopLength", "loopEnd"}},
        {"Launch", {"launchMode", "quantization", "legato", "followA", "followB", "followChanceA", "followTime"}},
        {"Signature", {"timeSigNum", "timeSigDen"}},
        {"Quantize", {"set/quantizeGrid", "set/quantizeAmount", "set/fixedRecordBars"}},
    };
    return list;
}

std::uint32_t colourOf(const juce::ValueTree& node, std::uint32_t fallback)
{
    const auto argb = static_cast<std::uint32_t>(static_cast<juce::int64>(node.getProperty(ids::colour, 0)));
    return (argb >> 24) == 0 ? fallback : rgbOf(argb);
}
} // namespace

ClipMode::ClipMode(ModeHost& h) : Mode(h) {}

int ClipMode::track() const
{
    return selectedTrack(host.context());
}

juce::ValueTree ClipMode::clip() const
{
    return selectedClip(host.context(), track());
}

int ClipMode::controls() const
{
    return std::max(1, padsOwned() ? caps().columns : caps().encoders);
}

std::vector<ClipMode::Page> ClipMode::pages() const
{
    // Each group starts a new page unless it fits after the one before; one too big is split.
    const int width = controls();
    std::vector<Page> out;
    for (const auto& g : groups())
    {
        const int size = static_cast<int>(g.values.size());
        if (out.empty() || static_cast<int>(out.back().values.size()) + size > width)
            out.push_back({});
        for (int i = 0; i < size; ++i)
        {
            if (static_cast<int>(out.back().values.size()) == width)
                out.push_back({});
            auto& p = out.back();
            if (p.values.empty() || !p.name.endsWith(g.name))
                p.name = p.name.isEmpty() ? juce::String(g.name) : p.name + dot + g.name;
            p.values.push_back(g.values[static_cast<std::size_t>(i)]);
        }
    }
    if (out.size() == 1)
        out.front().name = "Clip";
    return out;
}

int ClipMode::page() const
{
    return juce::jlimit(0, std::max(0, static_cast<int>(pages().size()) - 1), shownPage);
}

void ClipMode::setPage(int p)
{
    shownPage = juce::jlimit(0, std::max(0, static_cast<int>(pages().size()) - 1), p);
    host.requestRefresh();
}

juce::String ClipMode::targetFor(const juce::String& value) const
{
    if (value.startsWith("set/"))
        return value;
    const auto c = clip();
    return c.isValid() ? ui::clipValues::target(c, value) : juce::String();
}

const std::vector<std::pair<ClipMode::Action, juce::String>>& ClipMode::actions()
{
    static const std::vector<std::pair<Action, juce::String>> list{
        {Action::duplicateLoop, "Double Loop"}, {Action::doubleLoop, "Loop x2"}, {Action::halveLoop, "Loop / 2"},
        {Action::quantize, "Quantize"},         {Action::duplicate, "Duplicate"}};
    return list;
}

void ClipMode::perform(Action action)
{
    auto& session = host.context().session;
    const auto c = clip();
    if (!c.isValid())
        return;
    switch (action)
    {
    case Action::duplicateLoop: ui::clipActions::duplicateLoop(session, c); break;
    case Action::doubleLoop: ui::clipActions::doubleLoop(session, c); break;
    case Action::halveLoop: ui::clipActions::halveLoop(session, c); break;
    case Action::quantize: ui::clipActions::quantize(session, c); break;
    case Action::duplicate:
    {
        const auto [t, s] = session.set().findClip(LiveSet::idOf(c));
        session.beginEdit("Duplicate clip", c);
        if (const int to = session.set().duplicateClip(t, s); to >= 0 && host.followsSelection())
            host.context().selection.set({ui::SessionSelection::Kind::slot, t, to});
        break;
    }
    }
    host.requestRefresh();
}

// --- Input ------------------------------------------------------------------------------------------

bool ClipMode::handle(const SurfaceInput& input)
{
    using Kind = SurfaceInput::Kind;
    if (input.kind == Kind::buttonDown)
    {
        const auto id = input.button;
        switch (id.role)
        {
        case Role::displayUpper: setPage(id.index); return true;
        case Role::displayLower:
            if (id.index < static_cast<int>(actions().size()))
                perform(actions()[static_cast<std::size_t>(id.index)].first);
            return true;
        case Role::scene:
            if (!padsOwned())
                return false;
            setPage(id.index);
            return true;
        default: return false;
        }
    }
    if (input.kind == Kind::buttonUp)
        return input.button.role == Role::displayUpper || input.button.role == Role::displayLower ||
               (padsOwned() && input.button.role == Role::scene);
    if (!padsOwned())
        return false;
    if (input.kind == Kind::padDown)
    {
        if (input.row == caps().rows - 1 && caps().rows > 1)
        {
            // The bottom row: the actions.
            if (input.column < static_cast<int>(actions().size()))
                perform(actions()[static_cast<std::size_t>(input.column)].first);
        }
        else if (auto* binding = host.encoders().binding(input.column))
        {
            binding->setNormalised(faders::positionOfRow(input.row, faderRows())); // a step of its own
        }
        return true;
    }
    return input.kind == Kind::padUp || input.kind == Kind::padPressure;
}

// --- Output -----------------------------------------------------------------------------------------

void ClipMode::encoderTargets(std::vector<juce::String>& targets)
{
    const auto all = pages();
    if (all.empty())
        return;
    for (const auto& v : all[static_cast<std::size_t>(page())].values)
        targets.push_back(targetFor(v));
}

void ClipMode::display(DisplayFrame& f)
{
    const auto c = clip();
    const int t = track();
    const auto& set = host.context().session.set();
    if (c.isValid())
    {
        const auto name = c[ids::name].toString();
        f.title = set.track(t)[ids::name].toString() + juce::String::fromUTF8(" \xe2\x80\xba ") + (name.isNotEmpty() ? name : juce::String("Clip"));
    }
    else
    {
        f.title = t >= 0 ? set.track(t)[ids::name].toString() : juce::String("Clip");
    }
    const auto all = pages();
    for (int i = 0; i < caps().countOf(Role::displayUpper) && i < static_cast<int>(all.size()); ++i)
        f.upper.push_back({all[static_cast<std::size_t>(i)].name, 0u, i == page()});

    auto& bank = host.encoders();
    bool any = false;
    for (int i = 0; i < caps().encoders; ++i)
    {
        DisplayCell cell;
        if (auto* b = bank.binding(i))
        {
            cell = cellFor(*b);
            any = true;
        }
        f.cells.push_back(std::move(cell));
    }
    f.detail = bank.touchedEncoder();
    if (!c.isValid())
        f.message = any ? juce::String() : juce::String("No clip selected");
    if (c.isValid())
        for (int i = 0; i < caps().countOf(Role::displayLower) && i < static_cast<int>(actions().size()); ++i)
            f.lower.push_back({actions()[static_cast<std::size_t>(i)].second, 0u, false});
}

void ClipMode::describe(SurfaceText& text)
{
    const auto c = clip();
    const auto all = pages();
    text.status = c.isValid() ? "Clip: " + (c[ids::name].toString().isNotEmpty() ? c[ids::name].toString() : juce::String("(no name)"))
                              : juce::String("Clip: no clip selected");
    if (!all.empty())
        text.status << dot << all[static_cast<std::size_t>(page())].name << " (" << (page() + 1) << "/" << static_cast<int>(all.size()) << ")";
    if (padsOwned())
    {
        // The bottom row's actions.
        text.pads.assign(static_cast<std::size_t>(caps().numPads()), {});
        if (caps().rows > 1)
            for (int i = 0; i < caps().columns && i < static_cast<int>(actions().size()); ++i)
                text.pads[static_cast<std::size_t>((caps().rows - 1) * caps().columns + i)] = actions()[static_cast<std::size_t>(i)].second;
    }
}

void ClipMode::render(LedFrame& frame)
{
    const auto& colours = host.colours();
    const auto available = LedState::solid(dimmed(colours.white, colours.dim));
    const auto lit = LedState::solid(colours.white);
    const auto all = pages();
    const bool hasClip = clip().isValid();
    for (int i = 0; i < caps().countOf(Role::displayUpper); ++i)
        setButton(frame, {Role::displayUpper, i}, i < static_cast<int>(all.size()) ? (i == page() ? lit : available) : LedState::off());
    for (int i = 0; i < caps().countOf(Role::displayLower); ++i)
        setButton(frame, {Role::displayLower, i}, hasClip && i < static_cast<int>(actions().size()) ? available : LedState::off());
    if (!padsOwned())
        return;

    // The grid's columns as faders above a row of the actions, in the clip's colour.
    const auto c = clip();
    const int t = track();
    const auto colour = c.isValid() ? colourOf(c, colours.white) : t >= 0 ? colourOf(host.context().session.set().track(t), colours.white) : colours.white;
    for (int column = 0; column < caps().columns; ++column)
        if (const auto* b = host.encoders().binding(column))
            faders::renderColumn(frame, caps(), column, b->normalised(), cellFor(*b).bipolar, colour, 0.12f, faderRows());
    if (caps().rows > 1 && hasClip)
        for (int i = 0; i < caps().columns && i < static_cast<int>(actions().size()); ++i)
            frame.pad(caps().rows - 1, i, caps().columns) = available;
    const int scenes = caps().countOf(Role::scene);
    for (int i = 0; i < scenes; ++i)
        setButton(frame, {Role::scene, i}, i < static_cast<int>(all.size()) ? (i == page() ? lit : available) : LedState::off());
}

} // namespace lattice::control
