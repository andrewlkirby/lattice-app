#include "ui/browser/BrowserModel.h"

#include <algorithm>

namespace lattice::ui
{

using library::Item;
using library::Kind;
using library::Query;

juce::StringArray BrowserModel::categories()
{
    return {library::category::instruments, library::category::drums,   library::category::audioEffects,
            library::category::midiEffects, library::category::samples, presetsCategory,
            library::category::tunings};
}

BrowserModel::BrowserModel(library::Library& l, JobPool& j) : library(l), jobs(j)
{
    shown.kind = Source::Kind::category;
    shown.category = library::category::instruments;
    shown.name = shown.category;
    library.addChangeListener(this);
    run();
}

BrowserModel::~BrowserModel()
{
    library.removeChangeListener(this);
    if (pending != 0)
        jobs.cancel(pending);
}

std::vector<BrowserModel::Source> BrowserModel::sources() const
{
    std::vector<Source> result;
    for (int c = 0; c < library::LibraryIndex::numCollections; ++c)
        result.push_back({Source::Kind::collection, {}, c, {}, library::LibraryIndex::collectionName(c)});
    for (const auto& c : categories())
        result.push_back({Source::Kind::category, c, -1, {}, c});
    for (const auto& place : library.places())
        result.push_back({Source::Kind::place, {}, -1, place, place == library.userLibrary() ? "User Library" : place.getFileName()});
    return result;
}

void BrowserModel::setSource(const Source& source)
{
    if (source == shown && !searching())
        return;
    shown = source;
    searchText.clear();
    selectedKey.clear();
    selected = -1;
    run();
}

void BrowserModel::setSearch(const juce::String& text)
{
    if (text == searchText)
        return;
    searchText = text;
    run();
}

const BrowserModel::Row* BrowserModel::selectedItem() const
{
    return selected >= 0 && selected < static_cast<int>(list.size()) ? &list[static_cast<std::size_t>(selected)] : nullptr;
}

void BrowserModel::select(int row)
{
    row = list.empty() ? -1 : juce::jlimit(0, static_cast<int>(list.size()) - 1, row);
    if (row == selected)
        return;
    selected = row;
    selectedKey = row >= 0 ? list[static_cast<std::size_t>(row)].item.key : juce::String();
    if (onSelectionChanged)
        onSelectionChanged();
}

Query BrowserModel::queryFor() const
{
    Query q;
    if (searching())
    {
        q.text = searchText;
        return q;
    }
    switch (shown.kind)
    {
    case Source::Kind::collection:
        q.collection = shown.collection;
        break;
    case Source::Kind::place:
        q.folder = shown.place.getFullPathName();
        break;
    case Source::Kind::category:
        if (shown.category == presetsCategory)
            q.kind = static_cast<int>(Kind::preset);
        else
        {
            q.category = shown.category;
            // Devices (their presets open under them); the other categories are flat lists.
            if (shown.category == library::category::instruments || shown.category == library::category::drums ||
                shown.category == library::category::audioEffects || shown.category == library::category::midiEffects)
                q.kind = static_cast<int>(Kind::device);
        }
        break;
    }
    return q;
}

Query BrowserModel::childQuery(const Item& parent)
{
    Query q;
    if (parent.kind == Kind::folder)
        q.folder = parent.key;
    else
    {
        q.kind = static_cast<int>(Kind::preset);
        q.deviceType = parent.deviceType;
    }
    return q;
}

BrowserModel::Row BrowserModel::rowFor(const Item& item, int depth) const
{
    Row r;
    r.item = item;
    r.depth = depth;
    r.expandable = item.kind == Kind::folder || (item.kind == Kind::device && !searching());
    return r;
}

void BrowserModel::run()
{
    if (pending != 0)
        jobs.cancel(pending);
    const auto query = queryFor();
    const auto generation = ++queryGeneration;
    auto results = std::make_shared<std::vector<Item>>();
    // Children of what's open come with it, so a refresh keeps the tree as it was.
    auto children = std::make_shared<std::vector<std::pair<juce::String, std::vector<Item>>>>();
    auto* index = &library.index();
    const auto open = expandedKeys;
    std::weak_ptr<int> life = alive;
    pending = jobs.submit(
        "Browser search",
        [index, query, results, children, open](JobPool::Context& work) {
            *results = index->find(query);
            // Open folders and devices, as deep as they go.
            std::vector<Item> toOpen;
            for (const auto& i : *results)
                if (open.count(i.key) > 0)
                    toOpen.push_back(i);
            while (!toOpen.empty() && !work.isCancelled())
            {
                const auto parent = toOpen.back();
                toOpen.pop_back();
                auto kids = index->find(childQuery(parent));
                for (const auto& k : kids)
                    if (open.count(k.key) > 0)
                        toOpen.push_back(k);
                children->emplace_back(parent.key, std::move(kids));
            }
        },
        [this, life, generation, results, children](bool cancelled) {
            if (life.expired() || cancelled || generation != queryGeneration)
                return;
            pending = 0;
            deliver(std::move(*results));
            for (auto& [key, kids] : *children)
                deliverChildren(key, std::move(kids));
        });
}

void BrowserModel::discardRows()
{
    // A hundred thousand rows take milliseconds to free: a worker frees them, not the frame.
    if (list.size() > 1000)
        jobs.submit("Freeing browser rows", [old = std::make_shared<std::vector<Row>>(std::move(list))](JobPool::Context&) {
            old->clear();
        });
    list.clear();
}

void BrowserModel::deliver(std::vector<Item> items)
{
    discardRows();
    list.reserve(items.size());
    for (auto& i : items)
        list.push_back(rowFor(i, 0));
    // The selection stays on what was selected, if it's still there.
    selected = -1;
    if (selectedKey.isNotEmpty())
        for (std::size_t r = 0; r < list.size(); ++r)
            if (list[r].item.key == selectedKey)
                selected = static_cast<int>(r);
    if (onRowsChanged)
        onRowsChanged();
}

void BrowserModel::deliverChildren(const juce::String& parentKey, std::vector<Item> items)
{
    const auto at = std::find_if(list.begin(), list.end(), [&](const Row& r) { return r.item.key == parentKey; });
    if (at == list.end() || at->expanded)
        return;
    at->expanded = true;
    const int depth = at->depth + 1;
    if (items.empty() && at->item.kind == Kind::device)
        at->expandable = false; // a device without presets
    std::vector<Row> kids;
    kids.reserve(items.size());
    for (auto& i : items)
        kids.push_back(rowFor(i, depth));
    const auto position = static_cast<int>(std::distance(list.begin(), at)) + 1;
    list.insert(list.begin() + position, kids.begin(), kids.end());
    if (selected >= position)
        selected += static_cast<int>(kids.size());
    // The selection was inside: it's found again now its folder is open.
    if (selected < 0 && selectedKey.isNotEmpty())
        for (std::size_t k = 0; k < kids.size(); ++k)
            if (kids[k].item.key == selectedKey)
                selected = position + static_cast<int>(k);
    if (onRowsChanged)
        onRowsChanged();
}

void BrowserModel::loadChildren(const Item& parent)
{
    auto results = std::make_shared<std::vector<Item>>();
    auto* index = &library.index();
    const auto query = childQuery(parent);
    const auto key = parent.key;
    const auto generation = queryGeneration;
    std::weak_ptr<int> life = alive;
    jobs.submit("Browser folder", [index, query, results](JobPool::Context&) { *results = index->find(query); },
                [this, life, key, generation, results](bool cancelled) {
                    if (life.expired() || cancelled || generation != queryGeneration)
                        return;
                    deliverChildren(key, std::move(*results));
                });
}

void BrowserModel::setExpanded(int row, bool expand)
{
    if (row < 0 || row >= static_cast<int>(list.size()))
        return;
    auto& r = list[static_cast<std::size_t>(row)];
    if (!r.expandable || r.expanded == expand)
        return;
    if (expand)
    {
        expandedKeys.insert(r.item.key);
        loadChildren(r.item);
        return;
    }
    expandedKeys.erase(r.item.key);
    r.expanded = false;
    // Everything below it, deeper than it.
    auto end = row + 1;
    while (end < static_cast<int>(list.size()) && list[static_cast<std::size_t>(end)].depth > r.depth)
        ++end;
    const int removed = end - row - 1;
    list.erase(list.begin() + row + 1, list.begin() + end);
    if (selected > row && selected < end)
        select(row);
    else if (selected >= end)
        selected -= removed;
    if (onRowsChanged)
        onRowsChanged();
}

void BrowserModel::collapseOrParent(int row)
{
    if (row < 0 || row >= static_cast<int>(list.size()))
        return;
    const auto& r = list[static_cast<std::size_t>(row)];
    if (r.expanded)
    {
        setExpanded(row, false);
        return;
    }
    for (int up = row - 1; up >= 0; --up)
        if (list[static_cast<std::size_t>(up)].depth < r.depth)
        {
            select(up);
            return;
        }
}

void BrowserModel::refresh()
{
    run();
}

void BrowserModel::flushForTests()
{
    for (int i = 0; i < 20; ++i)
    {
        jobs.waitUntilIdle(5000);
        const bool wasIdle = jobs.idle(); // before delivering: what finishes in between comes next time
        if (jobs.deliverFinished() == 0 && wasIdle)
            break;
    }
}

void BrowserModel::setRowsForTests(std::vector<Row> rows)
{
    if (pending != 0)
        jobs.cancel(pending);
    ++queryGeneration;
    pending = 0;
    discardRows();
    list = std::move(rows);
    selected = -1;
    if (onRowsChanged)
        onRowsChanged();
}

} // namespace lattice::ui
