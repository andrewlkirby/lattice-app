#include "control/modes/BrowseMode.h"

#include <algorithm>

namespace lattice::control
{

namespace
{
/** How far either side of the selection a display's list is given (the renderer shows what fits). */
constexpr int listReach = 24;
const juce::String dot = juce::String::fromUTF8(" \xc2\xb7 ");

juce::String rowText(const ui::BrowserModel::Row& row)
{
    // Indented by depth; a folder (or a device with presets) marked open or shut.
    auto text = juce::String::repeatedString("  ", row.depth);
    if (row.expandable)
        text << (row.expanded ? juce::String::fromUTF8("\xe2\x96\xbe ") : juce::String::fromUTF8("\xe2\x96\xb8 "));
    return text + row.item.name;
}
} // namespace

BrowseMode::BrowseMode(ModeHost& h, BrowseHost& b) : Mode(h), browse(b)
{
    browse.addListener(this);
}

BrowseMode::~BrowseMode()
{
    browse.removeListener(this);
}

int BrowseMode::sourceColumns() const
{
    return std::max(1, caps().cellsAcross() / 4);
}

int BrowseMode::sourceIndex() const
{
    auto& model = browse.browser();
    const auto all = model.sources();
    const auto at = std::find(all.begin(), all.end(), model.source());
    return at == all.end() ? -1 : static_cast<int>(at - all.begin());
}

void BrowseMode::moveRow(int by)
{
    auto& model = browse.browser();
    const int n = static_cast<int>(model.rows().size());
    if (n == 0 || by == 0)
        return;
    model.select(juce::jlimit(0, n - 1, std::max(0, model.selectedRow()) + (model.selectedRow() < 0 ? 0 : by)));
    host.requestRefresh();
}

void BrowseMode::moveSource(int by)
{
    auto& model = browse.browser();
    const auto all = model.sources();
    if (all.empty() || by == 0)
        return;
    const int to = juce::jlimit(0, static_cast<int>(all.size()) - 1, std::max(0, sourceIndex()) + by);
    model.setSource(all[static_cast<std::size_t>(to)]);
    host.requestRefresh();
}

void BrowseMode::chooseCategory(int index)
{
    const auto names = ui::BrowserModel::categories();
    if (index < 0 || index >= names.size())
        return;
    auto& model = browse.browser();
    for (const auto& s : model.sources())
        if (s.kind == ui::BrowserModel::Source::Kind::category && s.category == names[index])
            model.setSource(s);
    host.requestRefresh();
}

const std::vector<std::pair<BrowseMode::Action, juce::String>>& BrowseMode::actions()
{
    static const std::vector<std::pair<Action, juce::String>> list{
        {Action::load, "Load"}, {Action::open, "Open"}, {Action::back, "Back"}, {Action::preview, "Preview"}, {Action::hotSwap, "Hot-Swap"}};
    return list;
}

void BrowseMode::perform(Action action)
{
    auto& model = browse.browser();
    const int row = model.selectedRow();
    switch (action)
    {
    case Action::load: browse.load(); break; // a folder opens, as Enter does
    case Action::open:
        if (const auto* r = model.selectedItem(); r != nullptr && r->expandable)
            model.setExpanded(row, !r->expanded);
        break;
    case Action::back: model.collapseOrParent(row); break;
    case Action::preview: browse.setPreviewOn(!browse.previewOn()); break;
    case Action::hotSwap: browse.setHotSwap(!browse.hotSwapTarget().isValid()); break;
    }
    host.requestRefresh();
}

int BrowseMode::firstGridRow() const
{
    // The selection kept on the grid, a page at a time.
    const int rows = std::max(1, caps().rows);
    const int selected = std::max(0, browse.browser().selectedRow());
    return selected / rows * rows;
}

// --- Input ------------------------------------------------------------------------------------------

bool BrowseMode::handle(const SurfaceInput& input)
{
    using Kind = SurfaceInput::Kind;
    switch (input.kind)
    {
    case Kind::encoderTurn:
    {
        // The encoders over the sources choose one; the others move along the rows.
        const int across = std::max(1, caps().cellsAcross());
        const int which = input.encoder % across < sourceColumns() ? 0 : 1;
        auto& p = pending[static_cast<std::size_t>(which)];
        p += input.delta;
        const int moves = p / stepsPerRow;
        p -= moves * stepsPerRow;
        const int by = moves * (host.shiftHeld() ? pageRows : 1);
        if (which == 0)
            moveSource(by);
        else
            moveRow(by);
        return true;
    }
    case Kind::encoderSet:
    {
        // An absolute control: its travel spans the list.
        const int across = std::max(1, caps().cellsAcross());
        auto& model = browse.browser();
        if (input.encoder % across < sourceColumns())
        {
            const auto all = model.sources();
            if (!all.empty())
                model.setSource(all[static_cast<std::size_t>(juce::roundToInt(input.value * static_cast<float>(all.size() - 1)))]);
        }
        else if (!model.rows().empty())
        {
            model.select(juce::roundToInt(input.value * static_cast<float>(model.rows().size() - 1)));
        }
        host.requestRefresh();
        return true;
    }
    case Kind::encoderTouch:
    case Kind::encoderRelease: return true;
    case Kind::buttonDown:
    {
        const auto id = input.button;
        const int page = host.shiftHeld() ? pageRows : 1;
        switch (id.role)
        {
        case Role::displayUpper: chooseCategory(id.index); return true;
        case Role::displayLower:
            if (id.index < static_cast<int>(actions().size()))
                perform(actions()[static_cast<std::size_t>(id.index)].first);
            return true;
        default: break;
        }
        if (!padsOwned())
            return false; // with encoders, the arrows and scene buttons are the pad mode's
        switch (id.role)
        {
        case Role::navUp: moveRow(-page); return true;
        case Role::navDown: moveRow(page); return true;
        case Role::navLeft: perform(Action::back); return true;
        case Role::navRight: perform(Action::open); return true;
        case Role::scene: chooseCategory(id.index); return true;
        default: return false;
        }
    }
    case Kind::buttonUp:
    {
        const auto role = input.button.role;
        return role == Role::displayUpper || role == Role::displayLower ||
               (padsOwned() && (role == Role::navUp || role == Role::navDown || role == Role::navLeft || role == Role::navRight ||
                                role == Role::scene));
    }
    case Kind::padDown:
    {
        if (!padsOwned())
            return false;
        auto& model = browse.browser();
        const int row = firstGridRow() + input.row;
        if (row >= static_cast<int>(model.rows().size()))
            return true;
        model.select(row);
        if (input.column == caps().columns - 1 && caps().columns > 1)
            perform(Action::load);
        host.requestRefresh();
        return true;
    }
    case Kind::padUp:
    case Kind::padPressure: return padsOwned();
    default: return false;
    }
}

// --- Output -----------------------------------------------------------------------------------------

void BrowseMode::display(DisplayFrame& f)
{
    auto& model = browse.browser();
    const auto swap = browse.hotSwapTarget();
    f.title = swap.isValid() ? "Hot-swap: " + swap[ids::name].toString() : juce::String("Browse");

    const auto names = ui::BrowserModel::categories();
    const auto& shown = model.source();
    for (int i = 0; i < caps().countOf(Role::displayUpper) && i < names.size(); ++i)
        f.upper.push_back({names[i], 0u, shown.kind == ui::BrowserModel::Source::Kind::category && shown.category == names[i]});

    // The sources, then the rows: each a window around its selection.
    const auto all = model.sources();
    DisplayList sources{"Sources", {}, -1, sourceColumns(), 0u};
    const int si = sourceIndex();
    const int sFirst = std::max(0, si - listReach);
    for (int i = sFirst; i < static_cast<int>(all.size()) && i <= si + listReach; ++i)
        sources.items.push_back(all[static_cast<std::size_t>(i)].name);
    sources.selected = si - sFirst;
    f.lists.push_back(std::move(sources));

    const auto& rows = model.rows();
    const int selected = model.selectedRow();
    DisplayList items{shown.name, {}, -1, std::max(1, caps().cellsAcross() - sourceColumns()), 0u};
    if (model.searching())
        items.title = "Search: " + model.search();
    const int first = std::max(0, selected - listReach);
    for (int i = first; i < static_cast<int>(rows.size()) && i <= std::max(selected, 0) + listReach; ++i)
        items.items.push_back(rowText(rows[static_cast<std::size_t>(i)]));
    items.selected = selected >= 0 ? selected - first : -1;
    if (rows.empty())
        items.items.push_back(model.loading() ? "Loading..." : "Nothing here");
    f.lists.push_back(std::move(items));

    const auto* r = model.selectedItem();
    for (int i = 0; i < caps().countOf(Role::displayLower) && i < static_cast<int>(actions().size()); ++i)
    {
        const auto [action, label] = actions()[static_cast<std::size_t>(i)];
        DisplayButton b{label, 0u, false};
        if (action == Action::open)
            b.label = r != nullptr && r->expanded ? "Close" : "Open";
        if (action == Action::preview)
            b.selected = browse.previewOn();
        if (action == Action::hotSwap)
            b.selected = swap.isValid();
        f.lower.push_back(b);
    }
}

void BrowseMode::describe(SurfaceText& text)
{
    auto& model = browse.browser();
    const auto* r = model.selectedItem();
    text.status = "Browse" + dot + model.source().name + (r != nullptr ? dot + r->item.name : juce::String());
    if (!padsOwned())
        return;
    // The rows on the grid: a row's name on its first pad, Load on its last.
    const int columns = caps().columns;
    text.pads.assign(static_cast<std::size_t>(caps().numPads()), {});
    const auto& rows = model.rows();
    for (int g = 0; g < caps().rows; ++g)
    {
        const int row = firstGridRow() + g;
        if (row >= static_cast<int>(rows.size()))
            break;
        text.pads[static_cast<std::size_t>(g * columns)] = rows[static_cast<std::size_t>(row)].item.name;
        if (columns > 1)
            text.pads[static_cast<std::size_t>(g * columns + columns - 1)] = "Load";
    }
}

void BrowseMode::render(LedFrame& frame)
{
    const auto& colours = host.colours();
    const auto available = LedState::solid(dimmed(colours.white, colours.dim));
    const auto lit = LedState::solid(colours.white);
    auto& model = browse.browser();
    const auto names = ui::BrowserModel::categories();
    const auto& shown = model.source();
    const auto upperLed = [&](int i) {
        if (i >= names.size())
            return LedState::off();
        return shown.kind == ui::BrowserModel::Source::Kind::category && shown.category == names[i] ? lit : available;
    };
    for (int i = 0; i < caps().countOf(Role::displayUpper); ++i)
        setButton(frame, {Role::displayUpper, i}, upperLed(i));
    for (int i = 0; i < caps().countOf(Role::displayLower); ++i)
    {
        LedState led = LedState::off();
        if (i < static_cast<int>(actions().size()))
        {
            const auto action = actions()[static_cast<std::size_t>(i)].first;
            led = available;
            if ((action == Action::preview && browse.previewOn()) || (action == Action::hotSwap && browse.hotSwapTarget().isValid()))
                led = lit;
        }
        setButton(frame, {Role::displayLower, i}, led);
    }
    if (!padsOwned())
        return; // the arrows are the pad mode's

    const int n = static_cast<int>(model.rows().size());
    const int selected = model.selectedRow();
    setButton(frame, {Role::navUp, 0}, selected > 0 ? available : LedState::off());
    setButton(frame, {Role::navDown, 0}, selected + 1 < n ? available : LedState::off());
    setButton(frame, {Role::navLeft, 0}, available);
    setButton(frame, {Role::navRight, 0}, model.selectedItem() != nullptr && model.selectedItem()->expandable ? available : LedState::off());

    // The rows on the grid: the selected one white, the others dim; each one's last pad loads it.
    const int columns = caps().columns;
    for (int g = 0; g < caps().rows; ++g)
    {
        const int row = firstGridRow() + g;
        if (row >= n)
            break;
        const bool isSelected = row == selected;
        for (int c = 0; c < columns; ++c)
            frame.pad(g, c, columns) = isSelected ? lit : LedState::solid(dimmed(colours.white, colours.padDim));
        if (columns > 1)
            frame.pad(g, columns - 1, columns) = LedState::solid(isSelected ? colours.play : dimmed(colours.play, colours.dim));
    }
    for (int i = 0; i < caps().countOf(Role::scene); ++i)
        setButton(frame, {Role::scene, i}, upperLed(i));
}

} // namespace lattice::control
