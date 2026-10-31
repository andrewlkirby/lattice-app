#pragma once

#include "core/JobPool.h"
#include "library/Library.h"

#include <juce_core/juce_core.h>

#include <functional>
#include <set>
#include <vector>

namespace lattice::ui
{

/** What the browser shows (docs/05-m4-ui.md, 4e), without drawing it: the sources down the side
    (collections, categories, places), the rows of the one chosen, or of a search, and which row
    is selected. Every query runs on the job pool (searching is disk work); rows arrive when it
    finishes, and the newest query wins. Message thread. */
class BrowserModel final : private juce::ChangeListener
{
public:
    struct Source
    {
        enum class Kind
        {
            collection,
            category,
            place
        };
        Kind kind = Kind::category;
        juce::String category;  // a category
        int collection = -1;    // a collection
        juce::File place;       // a place
        juce::String name;      // as shown

        bool operator==(const Source& o) const
        {
            return kind == o.kind && category == o.category && collection == o.collection && place == o.place;
        }
    };

    /** The categories, in the order the sidebar shows them. "Presets" lists every preset. */
    static constexpr const char* presetsCategory = "Presets";
    static juce::StringArray categories();

    struct Row
    {
        library::Item item;
        int depth = 0;
        bool expandable = false; // a folder, or a device that may have presets
        bool expanded = false;
    };

    BrowserModel(library::Library& library, JobPool& jobs);
    ~BrowserModel() override;

    std::vector<Source> sources() const;
    const Source& source() const noexcept { return shown; }
    void setSource(const Source& source);

    /** What's typed in the search box: it searches everything, ranked. Empty: the source's rows. */
    void setSearch(const juce::String& text);
    const juce::String& search() const noexcept { return searchText; }
    bool searching() const noexcept { return searchText.trim().isNotEmpty(); }

    const std::vector<Row>& rows() const noexcept { return list; }
    int selectedRow() const noexcept { return selected; }
    const Row* selectedItem() const;
    void select(int row);

    /** Opens or closes a folder (or a device's presets). */
    void setExpanded(int row, bool expanded);
    /** Closes the row's folder, or goes to the folder it's in: Left in the list. */
    void collapseOrParent(int row);

    /** Runs the queries again (the library changed). */
    void refresh();

    /** Called when the rows or the selection changed. */
    std::function<void()> onRowsChanged;
    std::function<void()> onSelectionChanged;

    /** True while a query is running. */
    bool loading() const noexcept { return pending != 0; }
    /** Tests: waits for the queries and delivers them. */
    void flushForTests();

    /** Tests: sets the rows directly, as a query would (the browser filling 100,000 results). */
    void setRowsForTests(std::vector<Row> rows);

private:
    void changeListenerCallback(juce::ChangeBroadcaster*) override { refresh(); }
    library::Query queryFor() const;
    void run();
    void loadChildren(const library::Item& parent);
    void deliver(std::vector<library::Item> items);
    void discardRows();
    void deliverChildren(const juce::String& parentKey, std::vector<library::Item> items);
    Row rowFor(const library::Item& item, int depth) const;
    static library::Query childQuery(const library::Item& parent);

    library::Library& library;
    JobPool& jobs;
    Source shown;
    juce::String searchText;
    std::vector<Row> list;
    int selected = -1;
    juce::String selectedKey;
    std::set<juce::String> expandedKeys;
    std::uint64_t pending = 0;
    std::uint64_t queryGeneration = 0;
    std::shared_ptr<int> alive = std::make_shared<int>(0);
};

} // namespace lattice::ui
