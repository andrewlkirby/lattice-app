#include "ui/tuning/TuningView.h"

#include "tuning/TrackTuning.h"
#include "tuning/parsers/ColundiTextParser.h"
#include "tuning/parsers/NumberParsing.h"
#include "ui/theme/LatticeLookAndFeel.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace lattice::ui
{
namespace
{
constexpr std::array<float, TuningView::numColumns> columnWeights{0.7f, 1.3f, 1.1f, 1.1f, 2.6f, 2.0f};

/** The columns' x positions across `width` (numColumns + 1 edges). */
std::array<int, TuningView::numColumns + 1> columnEdges(int width)
{
    float total = 0.0f;
    for (const float w : columnWeights)
        total += w;
    std::array<int, TuningView::numColumns + 1> edges{};
    float x = 0.0f;
    for (std::size_t c = 0; c < columnWeights.size(); ++c)
    {
        edges[c] = juce::roundToInt(x);
        x += static_cast<float>(width) * columnWeights[c] / total;
    }
    edges.back() = width;
    return edges;
}

int rowHeightFor(const juce::Component& c)
{
    return juce::roundToInt(themeOf(c).controlHeight);
}

/** A typed frequency: "440", "440 Hz", "1.2k", "1.2 kHz"; "-" (or "0") unmaps. */
std::optional<double> parseHz(const juce::String& typed)
{
    auto text = typed.trim().removeCharacters(" ").toLowerCase();
    if (text == "-")
        return 0.0;
    if (text.endsWith("hz"))
        text = text.dropLastCharacters(2);
    double scale = 1.0;
    if (text.endsWith("k"))
    {
        text = text.dropLastCharacters(1);
        scale = 1000.0;
    }
    const auto value = parsing::parseDecimal(text);
    if (!value || *value < 0.0 || !std::isfinite(*value))
        return std::nullopt;
    return *value * scale;
}
} // namespace

// --- Header: the column titles; a click sorts by one -------------------------------------------

class TuningView::Header final : public juce::Component
{
public:
    explicit Header(TuningView& v) : view(v) { setTitle("Columns"); }

    void paint(juce::Graphics& g) override
    {
        const auto& t = themeOf(*this);
        g.fillAll(t.surfaceRaised);
        g.setFont(t.font(t.fontSmall));
        const auto edges = columnEdges(getWidth());
        for (int c = 0; c < numColumns; ++c)
        {
            auto r = juce::Rectangle<int>(edges[static_cast<std::size_t>(c)], 0, edges[static_cast<std::size_t>(c) + 1] - edges[static_cast<std::size_t>(c)], getHeight())
                         .reduced(juce::roundToInt(t.padding * 0.5f), 0);
            const bool sorted = view.sortColumn() == static_cast<Column>(c);
            g.setColour(sorted ? t.text : t.textDim);
            auto name = columnName(static_cast<Column>(c));
            if (sorted)
                name << " " << juce::String::fromUTF8(view.sortAscending() ? "\xe2\x96\xb4" : "\xe2\x96\xbe"); // small up/down triangle
            g.drawText(name, r, juce::Justification::centredLeft, true);
        }
        g.setColour(t.border);
        g.fillRect(0, getHeight() - 1, getWidth(), 1);
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const auto edges = columnEdges(getWidth());
        for (int c = 0; c < numColumns; ++c)
            if (e.x >= edges[static_cast<std::size_t>(c)] && e.x < edges[static_cast<std::size_t>(c) + 1])
            {
                const auto column = static_cast<Column>(c);
                view.sortBy(column, view.sortColumn() == column ? !view.sortAscending() : true);
            }
    }

private:
    TuningView& view;
};

// --- Table: the rows -----------------------------------------------------------------------------

class TuningView::Table final : public juce::Component
{
public:
    explicit Table(TuningView& v) : view(v)
    {
        setTitle("Tuning table");
        setWantsKeyboardFocus(true);
        setHelpText("Click a row to play it. While editing a copy, double-click a frequency or a label to change it.");
    }

    int rowAt(int y) const
    {
        const int r = y / rowHeightFor(*this);
        return y >= 0 && r < static_cast<int>(view.rows().size()) ? r : -1;
    }

    juce::Rectangle<int> cellBounds(int row, Column c) const
    {
        const auto edges = columnEdges(getWidth());
        const int h = rowHeightFor(*this);
        const auto i = static_cast<std::size_t>(c);
        return {edges[i], row * h, edges[i + 1] - edges[i], h};
    }

    void paint(juce::Graphics& g) override
    {
        const auto& t = themeOf(*this);
        g.fillAll(t.surface);
        const auto tuning = view.shown();
        if (tuning == nullptr)
            return;
        const int h = rowHeightFor(*this);
        const auto clip = g.getClipBounds();
        const int first = juce::jmax(0, clip.getY() / h);
        const int last = juce::jmin(static_cast<int>(view.rows().size()) - 1, clip.getBottom() / h);
        const auto check = view.isEditing() ? view.problems() : tuning_edits::Check{};
        std::vector<int> problemOf(TuningTable::numNotes, 0); // 1 warning, 2 error
        for (const auto& p : check.problems)
            if (p.row >= 0)
                problemOf[static_cast<std::size_t>(p.row)] = juce::jmax(problemOf[static_cast<std::size_t>(p.row)], p.error ? 2 : 1);
        const double limit = view.playableLimit();
        g.setFont(t.font(t.fontNormal));
        for (int r = first; r <= last; ++r)
        {
            const int entry = view.rows()[static_cast<std::size_t>(r)];
            const auto row = juce::Rectangle<int>(0, r * h, getWidth(), h);
            const bool isChosen = view.chosen[static_cast<std::size_t>(entry)];
            const auto shade = [&](juce::Colour c) {
                g.setColour(c);
                g.fillRect(row);
            };
            if (isChosen)
                shade(t.selection.withAlpha(0.35f));
            else if (r % 2 == 1)
                shade(t.surfaceRaised.withAlpha(0.35f));
            if (view.playing >= 0 && entry == view.playing + view.noteOffset())
                shade(t.play.withAlpha(0.3f));

            const double hz = tuning->hz[static_cast<std::size_t>(entry)];
            for (int c = 0; c < numColumns; ++c)
            {
                const auto column = static_cast<Column>(c);
                auto cell = cellBounds(r, column).reduced(juce::roundToInt(t.padding * 0.5f), 0);
                juce::Colour colour = hz > 0.0 ? t.text : t.textDisabled;
                if (column == Column::index && tuning->isAnchor(entry))
                {
                    // Anchors (whole multiples of the base) get a dot, as on the piano roll's ruler.
                    const float d = static_cast<float>(h) * 0.3f;
                    g.setColour(t.accent);
                    g.fillEllipse(static_cast<float>(cell.getX()) + 2.0f, static_cast<float>(cell.getCentreY()) - d * 0.5f, d, d);
                }
                if (column == Column::hz && problemOf[static_cast<std::size_t>(entry)] == 2)
                    colour = t.error;
                if (column == Column::playable && hz > 0.0 && limit > 0.0 && hz >= limit)
                    colour = t.warning;
                if ((column == Column::playable || column == Column::index) && colour == t.text)
                    colour = t.textDim;
                g.setColour(colour);
                g.drawText(view.cellText(entry, column), cell,
                           column == Column::label || column == Column::playable ? juce::Justification::centredLeft
                                                                                   : juce::Justification::centredRight,
                           true);
            }
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        const int r = rowAt(e.y);
        if (r < 0)
            return;
        const int entry = view.rows()[static_cast<std::size_t>(r)];
        if (e.mods.isShiftDown() && anchorRow >= 0)
        {
            std::vector<int> range;
            for (int i = juce::jmin(anchorRow, r); i <= juce::jmax(anchorRow, r); ++i)
                range.push_back(view.rows()[static_cast<std::size_t>(i)]);
            view.selectEntries(range);
        }
        else if (e.mods.isCommandDown())
        {
            auto now = view.selectedEntries();
            if (const auto it = std::find(now.begin(), now.end(), entry); it != now.end())
                now.erase(it);
            else
                now.push_back(entry);
            view.selectEntries(now);
            anchorRow = r;
        }
        else
        {
            view.selectEntries({entry});
            anchorRow = r;
        }
        if (!e.mods.isPopupMenu())
            view.playEntry(entry, 100);
    }

    void mouseUp(const juce::MouseEvent&) override { view.playEntry(-1, 0); }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        const int r = rowAt(e.y);
        if (r < 0 || !view.isEditing())
            return;
        const auto edges = columnEdges(getWidth());
        const auto column = e.x >= edges[static_cast<std::size_t>(Column::label)] ? Column::label : Column::hz;
        view.editCell(view.rows()[static_cast<std::size_t>(r)], column);
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        const int count = static_cast<int>(view.rows().size());
        if (count == 0)
            return false;
        const int page = juce::jmax(1, (getParentHeight() / rowHeightFor(*this)) - 1);
        int r = anchorRow < 0 ? 0 : anchorRow;
        const int code = key.getKeyCode();
        if (code == juce::KeyPress::upKey)
            r -= 1;
        else if (code == juce::KeyPress::downKey)
            r += 1;
        else if (code == juce::KeyPress::pageUpKey)
            r -= page;
        else if (code == juce::KeyPress::pageDownKey)
            r += page;
        else if (code == juce::KeyPress::homeKey)
            r = 0;
        else if (code == juce::KeyPress::endKey)
            r = count - 1;
        else if (code == juce::KeyPress::returnKey && view.isEditing() && anchorRow >= 0)
        {
            view.editCell(view.rows()[static_cast<std::size_t>(anchorRow)], Column::hz);
            return true;
        }
        else
            return false;
        r = juce::jlimit(0, count - 1, r);
        anchorRow = r;
        view.selectEntries({view.rows()[static_cast<std::size_t>(r)]});
        scrollToRow(r);
        return true;
    }

    void scrollToRow(int r)
    {
        if (auto* port = findParentComponentOfClass<juce::Viewport>())
        {
            const int h = rowHeightFor(*this);
            const int top = port->getViewPositionY();
            if (r * h < top)
                port->setViewPosition(0, r * h);
            else if ((r + 1) * h > top + port->getViewHeight())
                port->setViewPosition(0, (r + 1) * h - port->getViewHeight());
        }
    }

    /** The in-place editor over a cell. */
    void openEditor(int entry, Column column)
    {
        const auto& rows = view.rows();
        const auto it = std::find(rows.begin(), rows.end(), entry);
        if (it == rows.end())
            return;
        const int r = static_cast<int>(it - rows.begin());
        scrollToRow(r);
        editor = std::make_unique<juce::TextEditor>();
        editor->setTitle(columnName(column) + " of #" + juce::String(entry));
        editor->setText(column == Column::hz ? (view.shown()->hz[static_cast<std::size_t>(entry)] > 0.0
                                                    ? juce::String(view.shown()->hz[static_cast<std::size_t>(entry)], 6)
                                                          .trimCharactersAtEnd("0")
                                                          .trimCharactersAtEnd(".")
                                                    : juce::String("-"))
                                              : view.shown()->labels[static_cast<std::size_t>(entry)],
                         false);
        editor->setBounds(cellBounds(r, column));
        editor->selectAll();
        editing = {entry, column};
        editor->onReturnKey = [this] { commit(true); };
        editor->onEscapeKey = [this] { close(true); };
        editor->onFocusLost = [this] { commit(false); };
        editor->onTextChange = [this] { editor->setColour(juce::TextEditor::textColourId, themeOf(*this).text); };
        addAndMakeVisible(*editor);
        editor->grabKeyboardFocus();
    }

    juce::TextEditor* cellEditor() noexcept { return editor.get(); }

private:
    void commit(bool fromReturn)
    {
        if (editor == nullptr)
            return;
        const auto [entry, column] = editing;
        if (!view.setCell(entry, column, editor->getText()))
        {
            if (fromReturn)
                editor->setColour(juce::TextEditor::textColourId, themeOf(*this).error); // stays open to fix
            else
                close(false);
            return;
        }
        close(fromReturn); // a click elsewhere keeps the focus where it went
        // Return after a frequency goes on to the label, as in a spreadsheet's row.
        if (fromReturn && column == Column::hz)
            juce::MessageManager::callAsync([safe = juce::Component::SafePointer<Table>(this), e = entry] {
                if (safe != nullptr)
                    safe->openEditor(e, Column::label);
            });
    }

    void close(bool refocus)
    {
        if (editor == nullptr)
            return;
        auto old = std::move(editor);
        old->onFocusLost = nullptr;
        removeChildComponent(old.get());
        juce::MessageManager::callAsync([o = std::shared_ptr<juce::TextEditor>(std::move(old))] {}); // freed after its callback returns
        if (refocus)
            grabKeyboardFocus();
    }

    TuningView& view;
    int anchorRow = -1;
    std::unique_ptr<juce::TextEditor> editor;
    std::pair<int, Column> editing{-1, Column::hz};
};

// --- TuningView ----------------------------------------------------------------------------------

juce::String TuningView::columnName(Column c)
{
    switch (c)
    {
    case Column::index: return "#";
    case Column::hz: return "Hz";
    case Column::cents: return "Cents";
    case Column::ratio: return "Ratio";
    case Column::label: return "Label";
    case Column::playable: return "Plays";
    }
    return {};
}

TuningView::TuningView(SessionController& s, TuningRegistry& tunings, SessionSelectionModel& sel, Actions a)
    : session(s), registry(tunings), selection(sel), actions(std::move(a))
{
    setTitle("Tuning view");
    header = std::make_unique<Header>(*this);
    table = std::make_unique<Table>(*this);
    viewport.setViewedComponent(table.get(), false);
    viewport.setScrollBarsShown(true, false);
    viewport.setWantsKeyboardFocus(false);
    addAndMakeVisible(*header);
    addAndMakeVisible(viewport);

    const auto button = [this](juce::TextButton& b, const juce::String& help, std::function<void()> click) {
        b.setWantsKeyboardFocus(false);
        b.setTooltip(help);
        b.setHelpText(help);
        b.onClick = std::move(click);
        addAndMakeVisible(b);
    };
    button(duplicateBtn, "Makes a copy of this tuning to edit; the track plays the copy while it's open.", [this] { duplicate(); });
    button(editBtn, "Edits this tuning (one you saved from the editor).", [this] { editInPlace(); });
    button(loadBtn, "Loads a .scl or Colundi .txt tuning file for this track.", [this] {
        if (actions.loadFile && track() >= 0)
            actions.loadFile(track());
    });
    button(saveBtn, "Saves the copy to your Tunings folder and gives it to the track.", [this] { save(); });
    button(discardBtn, "Drops the copy; the track plays its own tuning again.", [this] { discard(); });
    button(shiftBtn, "Moves every entry up or down by a number of keys.", [this] {
        if (actions.ask)
            actions.ask("Shift", "Move every entry by how many keys? (negative: down)", "1", [this](const juce::String& text) {
                if (const auto keys = parsing::parseInteger(text.trim().trimCharactersAtStart("+")); keys)
                    shiftBy(static_cast<int>(*keys));
                else if (text.trim().startsWithChar('-'))
                    if (const auto down = parsing::parseInteger(text.trim().substring(1)); down)
                        shiftBy(-static_cast<int>(*down));
            });
    });
    button(multiplyBtn, "Multiplies every frequency by a ratio: 3/2, 1.25, or cents such as 700c.", [this] {
        if (actions.ask)
            actions.ask("Multiply", "Multiply every frequency by (3/2, 1.25, 700c):", "3/2", [this](const juce::String& text) {
                if (!multiplyBy(text) && actions.confirm)
                    actions.confirm("Multiply", "'" + text + "' isn't a ratio.", {}, nullptr);
            });
    });
    button(keepSelectedBtn, "Keeps only the selected entries, on consecutive keys around middle C.", [this] { keepSelected(); });
    button(keepAnchorsBtn, "Keeps only the anchors (whole multiples of the base), on consecutive keys around middle C.",
           [this] { keepAnchors(); });
    button(copyBtn, "Copies the rows shown (or the selected ones) as tab-separated text.", [this] { copyToClipboard(); });

    filterBox.setTitle("Filter");
    filterBox.setTextToShowWhenEmpty("Filter: index, Hz or label", themeOf(*this).textDim);
    filterBox.setHelpText("Shows the rows whose index, frequency or label start with every word typed.");
    filterBox.onTextChange = [this] { setFilter(filterBox.getText()); };
    filterBox.onEscapeKey = [this] {
        filterBox.clear();
        setFilter({});
    };
    addAndMakeVisible(filterBox);

    nameBox.setTitle("Tuning name");
    nameBox.setHelpText("The copy's name, and its file's.");
    const auto commitName = [this] {
        if (draft.isOpen() && nameBox.getText().trim().isNotEmpty() && nameBox.getText().trim() != draft.name())
            draft.setName(nameBox.getText());
        nameBox.setText(draft.name(), false);
    };
    nameBox.onReturnKey = commitName;
    nameBox.onFocusLost = commitName;
    addChildComponent(nameBox);

    anchorsToggle.setWantsKeyboardFocus(false);
    anchorsToggle.setHelpText("Shows only the anchors: whole multiples of the base frequency.");
    anchorsToggle.onClick = [this] { setAnchorsOnly(anchorsToggle.getToggleState()); };
    playableToggle.setWantsKeyboardFocus(false);
    playableToggle.setHelpText("Shows only the entries that play at the current sample rate.");
    playableToggle.onClick = [this] { setPlayableOnly(playableToggle.getToggleState()); };
    addAndMakeVisible(anchorsToggle);
    addAndMakeVisible(playableToggle);

    draft.onChange = [this] { draftChanged(); };

    selection.addListener(this);
    registry.addListener(this);
    session.set().state().addListener(this);
    selectionChanged(selection.get());
}

TuningView::~TuningView()
{
    playEntry(-1, 0);
    if (shownTrack != 0)
        session.snapshots().setTuningOverride(shownTrack, nullptr);
    session.set().state().removeListener(this);
    registry.removeListener(this);
    selection.removeListener(this);
}

juce::Component& TuningView::tableComponent()
{
    return *table;
}

int TuningView::indexOfTrack(std::int64_t id) const
{
    const auto& set = session.set();
    for (int t = 0; t < set.numTracks(); ++t)
        if (set.trackId(t) == id)
            return t;
    return -1;
}

int TuningView::track() const
{
    const int t = indexOfTrack(shownTrack);
    return t >= 0 && !session.set().isGroupTrack(t) ? t : -1;
}

int TuningView::noteOffset() const
{
    const int t = track();
    return t >= 0 ? static_cast<int>(session.set().track(t)[ids::noteOffset]) : 0;
}

TuningPtr TuningView::shown() const
{
    if (draft.isOpen())
        return draft.table();
    const int t = track();
    if (t < 0)
        return nullptr;
    const auto found = registry.findById(session.set().track(t)[ids::tuningId].toString());
    return found != nullptr ? found : registry.defaultTuning();
}

double TuningView::sampleRate() const
{
    return actions.sampleRate ? actions.sampleRate() : 0.0;
}

double TuningView::playableLimit() const
{
    return sampleRate() * TrackTuning::maxPlayableFraction;
}

// --- Following the selection and the set ---------------------------------------------------------

void TuningView::selectionChanged(const SessionSelection& now)
{
    if (draft.isOpen())
        return; // the copy stays with its track
    const auto& set = session.set();
    if (now.kind == SessionSelection::Kind::scene || now.track < 0 || now.track >= set.numTracks())
        return;
    const auto id = set.trackId(now.track);
    if (id == shownTrack && controls != nullptr)
        return;
    shownTrack = id;
    refresh();
}

void TuningView::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree.hasType(ids::track) && (property == ids::tuningId || property == ids::noteOffset || property == ids::highNotePolicy ||
                                     property == ids::name))
        triggerAsyncUpdate();
}

void TuningView::setReplaced()
{
    discard(true);
    shownTrack = 0;
    selectionChanged(selection.get());
    refresh();
}

void TuningView::handleAsyncUpdate()
{
    // The copy's track was deleted (or a different set loaded): the copy goes with it.
    if (draft.isOpen() && indexOfTrack(shownTrack) < 0)
        discard(true);
    if (indexOfTrack(shownTrack) < 0)
    {
        shownTrack = 0;
        selectionChanged(selection.get());
    }
    refresh();
}

void TuningView::refresh()
{
    const int t = track();
    if (controls == nullptr || controls->track() != t)
    {
        if (controls != nullptr)
            removeChildComponent(controls.get());
        controls = t >= 0 ? std::make_unique<TrackTuningControls>(session, registry, t, TrackTuningControls::Layout::row) : nullptr;
        if (controls != nullptr)
            addAndMakeVisible(*controls);
    }
    rebuildRows();
    updateButtons();
    resized();
    repaint();
}

void TuningView::rebuildRows()
{
    visible.clear();
    const auto tuning = shown();
    if (tuning == nullptr)
    {
        table->setSize(viewport.getMaximumVisibleWidth(), 0);
        return;
    }
    const double limit = playableLimit();
    for (int i = 0; i < TuningTable::numNotes; ++i)
    {
        const double hz = tuning->hz[static_cast<std::size_t>(i)];
        if (anchorsOnly && !tuning->isAnchor(i))
            continue;
        if (playableOnly && (hz <= 0.0 || (limit > 0.0 && hz >= limit)))
            continue;
        // Every word typed starts the index, the frequency or a word of the label.
        const auto labelWords = juce::StringArray::fromTokens(tuning->labels[static_cast<std::size_t>(i)].toLowerCase(), " ", "");
        bool matches = true;
        for (const auto& word : filterWords)
        {
            bool hit = juce::String(i).startsWith(word) || (hz > 0.0 && juce::String(hz, 3).startsWith(word));
            for (const auto& w : labelWords)
                hit = hit || w.startsWith(word);
            matches = matches && hit;
        }
        if (matches)
            visible.push_back(i);
    }
    const auto key = [&](int i) -> double {
        const double hz = tuning->hz[static_cast<std::size_t>(i)];
        switch (sortedBy)
        {
        case Column::index: return i;
        case Column::hz:
        case Column::cents:
        case Column::ratio: return hz > 0.0 ? hz : -1.0; // unmapped first
        case Column::playable: return hz <= 0.0 ? 3.0 : (limit > 0.0 && hz >= limit ? 2.0 : 1.0);
        case Column::label: return 0.0;
        }
        return i;
    };
    std::stable_sort(visible.begin(), visible.end(), [&](int a, int b) {
        if (sortedBy == Column::label)
        {
            const int c = tuning->labels[static_cast<std::size_t>(a)].compareNatural(tuning->labels[static_cast<std::size_t>(b)]);
            return ascending ? c < 0 : c > 0;
        }
        const double ka = key(a), kb = key(b);
        if (ka == kb)
            return a < b;
        return ascending ? ka < kb : ka > kb;
    });
    table->setSize(viewport.getMaximumVisibleWidth(), static_cast<int>(visible.size()) * rowHeightFor(*this));
    table->repaint();
    header->repaint();
}

void TuningView::sortBy(Column column, bool up)
{
    sortedBy = column;
    ascending = up;
    rebuildRows();
}

void TuningView::setFilter(const juce::String& text)
{
    filterWords = juce::StringArray::fromTokens(text.toLowerCase(), " ", "");
    filterWords.removeEmptyStrings();
    rebuildRows();
}

void TuningView::setAnchorsOnly(bool on)
{
    anchorsOnly = on;
    anchorsToggle.setToggleState(on, juce::dontSendNotification);
    rebuildRows();
}

void TuningView::setPlayableOnly(bool on)
{
    playableOnly = on;
    playableToggle.setToggleState(on, juce::dontSendNotification);
    rebuildRows();
}

juce::String TuningView::cellText(int entry, Column column) const
{
    const auto tuning = shown();
    if (tuning == nullptr || entry < 0 || entry >= TuningTable::numNotes)
        return {};
    const double hz = tuning->hz[static_cast<std::size_t>(entry)];
    const double base = tuning->baseHz();
    switch (column)
    {
    case Column::index: return juce::String(entry);
    case Column::hz: return hz > 0.0 ? juce::String(hz, 3) : juce::String("-");
    case Column::cents: return hz > 0.0 && base > 0.0 ? juce::String(1200.0 * std::log2(hz / base), 1) : juce::String();
    case Column::ratio: return hz > 0.0 && base > 0.0 ? juce::String(hz / base, 4) : juce::String();
    case Column::label: return tuning->labels[static_cast<std::size_t>(entry)];
    case Column::playable:
    {
        if (hz <= 0.0)
            return "Unmapped";
        const double limit = playableLimit();
        if (limit <= 0.0 || hz < limit)
            return "Yes";
        const int t = track();
        const bool folds = t >= 0 && static_cast<int>(session.set().track(t)[ids::highNotePolicy]) == 1;
        if (!folds || tuning->periodRatio <= 1.0)
            return "Muted (too high)";
        double folded = hz;
        while (folded >= limit)
            folded /= tuning->periodRatio;
        return "Folds to " + (folded >= 1000.0 ? juce::String(folded / 1000.0, 1) + " kHz" : juce::String(folded, 1) + " Hz");
    }
    }
    return {};
}

juce::String TuningView::tableText(bool selectedOnly) const
{
    juce::String out;
    for (int c = 0; c < numColumns; ++c)
        out << (c > 0 ? "\t" : "") << columnName(static_cast<Column>(c));
    out << "\n";
    for (const int entry : visible)
    {
        if (selectedOnly && !chosen[static_cast<std::size_t>(entry)])
            continue;
        for (int c = 0; c < numColumns; ++c)
            out << (c > 0 ? "\t" : "") << cellText(entry, static_cast<Column>(c));
        out << "\n";
    }
    return out;
}

void TuningView::copyToClipboard()
{
    juce::SystemClipboard::copyTextToClipboard(tableText(!selectedEntries().empty()));
}

std::vector<int> TuningView::selectedEntries() const
{
    std::vector<int> out;
    for (int i = 0; i < TuningTable::numNotes; ++i)
        if (chosen[static_cast<std::size_t>(i)])
            out.push_back(i);
    return out;
}

void TuningView::selectEntries(const std::vector<int>& entries)
{
    std::fill(chosen.begin(), chosen.end(), false);
    for (const int e : entries)
        if (e >= 0 && e < TuningTable::numNotes)
            chosen[static_cast<std::size_t>(e)] = true;
    updateButtons();
    table->repaint();
}

void TuningView::playEntry(int entry, int velocity)
{
    const int t = track();
    if (playing >= 0)
    {
        if (t >= 0)
            session.previewNote(t, playing, 0);
        playing = -1;
        table->repaint();
    }
    if (velocity <= 0 || t < 0 || entry < 0)
        return;
    // The key that plays this entry: the track's note offset moves keys along the table.
    const int key = entry - noteOffset();
    if (key < 0 || key >= TuningTable::numNotes)
        return;
    session.previewNote(t, key, velocity);
    playing = key;
    table->repaint();
}

// --- Editing -------------------------------------------------------------------------------------

juce::String TuningView::uniqueName(const juce::String& wanted) const
{
    const auto taken = [&](const juce::String& n) {
        if (actions.userTunings.getChildFile(juce::File::createLegalFileName(n) + ".txt").exists())
            return true;
        for (const auto& t : registry.getAll())
            if (t->name == n)
                return true;
        return false;
    };
    if (!taken(wanted))
        return wanted;
    for (int n = 2;; ++n)
        if (const auto name = wanted + " " + juce::String(n); !taken(name))
            return name;
}

void TuningView::beginDraft(const TuningTable& from, const juce::String& name, const juce::File& file, const juce::String& basedOn)
{
    if (track() < 0)
        return;
    playEntry(-1, 0);
    draft.begin(from, name, file, basedOn);
    nameBox.setText(name, false);
    auto* undo = &draft.history();
    const auto tree = draft.state();
    periodBox = std::make_unique<NumberBox>(ValueBinding::property(
        ParameterSpec::continuous("tuningPeriod", "Period", 1.001f, 10000.0f, 2.0f, ParamUnit::Ratio).withCentre(2.0f).inGroup("Tuning Editor"),
        undo, tree, TuningDraft::periodId, [this](double v) { draft.setPeriod(v); }));
    periodBox->setTooltip("The tuning's repeat interval: high notes fold down by it (2 is an octave; Colundi's is 938.667).");
    pageBox = std::make_unique<NumberBox>(ValueBinding::property(
        ParameterSpec::integer("tuningPage", "Keyboard Page", 1, TuningTable::numNotes, 12).inGroup("Tuning Editor"), undo, tree,
        TuningDraft::pageId, [this](double v) { draft.setKeyboardPage(juce::roundToInt(v)); }));
    pageBox->setTooltip("How many keys the computer keyboard's Z and X move by.");
    addAndMakeVisible(*periodBox);
    addAndMakeVisible(*pageBox);
    draftChanged();
    refresh();
}

void TuningView::duplicate()
{
    const auto from = shown();
    if (from == nullptr || track() < 0)
        return;
    if (draft.isOpen())
        return;
    beginDraft(*from, uniqueName(from->name + " copy"), {}, {});
}

bool TuningView::canEditInPlace() const
{
    const auto t = draft.isOpen() ? nullptr : shown();
    if (t == nullptr || !juce::File::isAbsolutePath(t->sourceDescription))
        return false;
    const juce::File file(t->sourceDescription);
    return file.hasFileExtension("txt") && file.isAChildOf(actions.userTunings);
}

void TuningView::editInPlace()
{
    if (!canEditInPlace())
        return;
    const auto t = shown();
    beginDraft(*t, t->name, juce::File(t->sourceDescription), t->id);
}

tuning_edits::Check TuningView::problems() const
{
    const auto t = draft.isOpen() ? draft.table() : shown();
    return t != nullptr ? tuning_edits::check(*t, sampleRate()) : tuning_edits::Check{};
}

void TuningView::draftChanged()
{
    // The track plays the copy while it's open, as long as it can be played.
    if (draft.isOpen() && problems().ok())
        session.snapshots().setTuningOverride(shownTrack, draft.table());
    nameBox.setText(draft.name(), false);
    rebuildRows();
    updateButtons();
    repaint();
}

bool TuningView::setCell(int entry, Column column, const juce::String& text)
{
    if (!draft.isOpen() || entry < 0 || entry >= TuningTable::numNotes)
        return false;
    if (column == Column::hz)
    {
        const auto hz = parseHz(text);
        if (!hz)
            return false;
        if (*hz != draft.table()->hz[static_cast<std::size_t>(entry)])
            draft.setHz(entry, *hz);
        return true;
    }
    if (column == Column::label)
    {
        const auto usual = ColundiTextParser::labelFor(entry, draft.table()->hz[static_cast<std::size_t>(entry)]);
        const auto label = text.trim() == usual ? juce::String() : text.trim();
        if (label != draft.state().getChild(entry)[TuningDraft::labelId].toString())
            draft.setLabel(entry, label);
        return true;
    }
    return false;
}

void TuningView::editCell(int entry, Column column)
{
    if (draft.isOpen())
        table->openEditor(entry, column);
}

void TuningView::shiftBy(int keys)
{
    if (draft.isOpen() && keys != 0)
        draft.replaceEntries(tuning_edits::shifted(*draft.table(), keys), "Shift Tuning");
}

bool TuningView::multiplyBy(const juce::String& text)
{
    const auto ratio = tuning_edits::parseRatio(text);
    if (!draft.isOpen() || !ratio)
        return false;
    draft.replaceEntries(tuning_edits::multiplied(*draft.table(), *ratio), "Multiply Tuning");
    return true;
}

void TuningView::keepSelected()
{
    const auto entries = selectedEntries();
    if (!draft.isOpen() || entries.empty())
        return;
    draft.replaceEntries(tuning_edits::subset(*draft.table(), entries, tuning_edits::centredStart(static_cast<int>(entries.size()))),
                         "Keep Selected");
    selectEntries({});
}

void TuningView::keepAnchors()
{
    if (!draft.isOpen())
        return;
    const auto anchors = tuning_edits::anchorIndices(*draft.table());
    if (anchors.empty())
        return;
    draft.replaceEntries(tuning_edits::subset(*draft.table(), anchors, tuning_edits::centredStart(static_cast<int>(anchors.size()))),
                         "Keep Anchors");
    selectEntries({});
}

void TuningView::unmapSelected()
{
    if (!draft.isOpen())
        return;
    auto edited = *draft.table();
    for (const int e : selectedEntries())
    {
        edited.hz[static_cast<std::size_t>(e)] = 0.0;
        edited.labels[static_cast<std::size_t>(e)] = ColundiTextParser::labelFor(e, 0.0);
    }
    draft.replaceEntries(edited, "Unmap Entries");
}

bool TuningView::save()
{
    if (!draft.isOpen())
        return false;
    const auto check = problems();
    const auto tell = [this](const juce::String& message) {
        if (actions.confirm)
            actions.confirm("Can't save the tuning", message, {}, nullptr);
    };
    if (!check.ok())
    {
        for (const auto& p : check.problems)
            if (p.error)
            {
                tell((p.row >= 0 ? "#" + juce::String(p.row) + ": " : juce::String()) + p.message);
                break;
            }
        return false;
    }
    const auto name = draft.name().trim();
    if (name.isEmpty())
    {
        tell("The tuning needs a name.");
        return false;
    }
    const auto file = actions.userTunings.getChildFile(juce::File::createLegalFileName(name) + ".txt");
    if (file.exists() && file != draft.file())
    {
        if (actions.confirm)
            actions.confirm("Replace " + file.getFileName() + "?", "A tuning file with that name is already in your Tunings folder.",
                            "Replace", [this, file] { writeAndUse(file); });
        return false;
    }
    return writeAndUse(file);
}

bool TuningView::writeAndUse(const juce::File& file)
{
    const int t = track();
    if (!draft.isOpen() || t < 0)
        return false;
    auto saved = *draft.table();
    saved.name = draft.name().trim();
    if (!actions.userTunings.createDirectory() || !file.replaceWithText(ColundiTextParser::write(saved)))
    {
        if (actions.confirm)
            actions.confirm("Can't save the tuning", "Couldn't write " + file.getFullPathName() + ".", {}, nullptr);
        return false;
    }
    const auto loaded = registry.loadFile(file);
    if (!loaded.ok())
    {
        if (actions.confirm)
            actions.confirm("Can't save the tuning", loaded.error, {}, nullptr);
        return false;
    }
    // The track gets the saved tuning, and so does every track that had the version it replaces.
    auto& set = session.set();
    const auto replaced = draft.basedOn();
    session.undoManager().beginNewTransaction("Save Tuning");
    for (int other = 0; other < set.numTracks(); ++other)
        if (other == t || (replaced.isNotEmpty() && set.track(other)[ids::tuningId].toString() == replaced))
            set.setTrackTuning(other, loaded.table->id);
    discard(true);
    return true;
}

void TuningView::discard(bool force)
{
    if (!draft.isOpen())
        return;
    if (!force && draft.isModified() && actions.confirm)
    {
        actions.confirm("Discard the changes?", "The copy of the tuning hasn't been saved.", "Discard", [this] { discard(true); });
        return;
    }
    playEntry(-1, 0);
    draft.end();
    session.snapshots().setTuningOverride(shownTrack, nullptr);
    for (auto* box : {periodBox.get(), pageBox.get()})
        if (box != nullptr)
            removeChildComponent(box);
    periodBox.reset();
    pageBox.reset();
    selectionChanged(selection.get()); // back to following the selection
    refresh();
}

void TuningView::updateButtons()
{
    const bool editing = draft.isOpen();
    const bool hasTrack = track() >= 0;
    duplicateBtn.setVisible(!editing);
    editBtn.setVisible(!editing);
    loadBtn.setVisible(!editing);
    duplicateBtn.setEnabled(hasTrack);
    editBtn.setEnabled(canEditInPlace());
    loadBtn.setEnabled(hasTrack);
    nameBox.setVisible(editing);
    for (auto* b : {&saveBtn, &discardBtn, &shiftBtn, &multiplyBtn, &keepSelectedBtn, &keepAnchorsBtn})
        b->setVisible(editing);
    saveBtn.setEnabled(editing && problems().ok());
    keepSelectedBtn.setEnabled(editing && !selectedEntries().empty());
}

// --- Commands ------------------------------------------------------------------------------------

EditTarget* TuningView::editTargetIfFocused()
{
    return isShowing() && table->hasKeyboardFocus(false) ? this : nullptr;
}

bool TuningView::canEdit(const juce::String& id) const
{
    if (id == "edit.copy" || id == "edit.selectAll")
        return shown() != nullptr;
    if (id == "edit.delete")
        return draft.isOpen() && std::find(chosen.begin(), chosen.end(), true) != chosen.end();
    if (id == "edit.undo")
        return draft.isOpen() && draft.history().canUndo();
    if (id == "edit.redo")
        return draft.isOpen() && draft.history().canRedo();
    return false;
}

void TuningView::edit(const juce::String& id)
{
    if (id == "edit.copy")
        copyToClipboard();
    else if (id == "edit.selectAll")
        selectEntries(visible);
    else if (id == "edit.delete")
        unmapSelected();
    else if (id == "edit.undo")
        draft.undoLast();
    else if (id == "edit.redo")
        draft.redoLast();
}

// --- Layout --------------------------------------------------------------------------------------

void TuningView::resized()
{
    const auto& t = themeOf(*this);
    auto area = getLocalBounds().reduced(juce::roundToInt(t.padding));
    const int h = juce::roundToInt(t.controlHeight);
    const int gap = juce::roundToInt(t.spacing);
    const auto take = [&](juce::Rectangle<int>& row, float units) {
        auto r = row.removeFromLeft(juce::roundToInt(t.controlHeight * units));
        row.removeFromLeft(gap);
        return r;
    };

    // Top: the name (or the copy's name box) and its buttons; the track's settings on the right.
    auto top = area.removeFromTop(h);
    area.removeFromTop(gap);
    if (controls != nullptr)
        controls->setBounds(top.removeFromRight(juce::jmin(controls->rowWidth(), top.getWidth() / 2)));
    const bool editing = draft.isOpen();
    if (editing)
    {
        nameBox.setBounds(take(top, 7.0f));
        saveBtn.setBounds(take(top, 3.0f));
        discardBtn.setBounds(take(top, 3.0f));
    }
    else
    {
        const auto buttons = top.removeFromRight(juce::roundToInt(t.controlHeight * 10.0f) + gap * 3);
        auto b = buttons;
        duplicateBtn.setBounds(take(b, 3.5f));
        editBtn.setBounds(take(b, 2.5f));
        loadBtn.setBounds(take(b, 4.0f));
    }
    titleArea = top;

    // The filter, and (editing) the tools.
    auto tools = area.removeFromTop(h);
    area.removeFromTop(gap);
    filterBox.setBounds(take(tools, 7.0f));
    anchorsToggle.setBounds(take(tools, 3.0f));
    playableToggle.setBounds(take(tools, 3.0f));
    copyBtn.setBounds(take(tools, 2.5f));
    if (editing)
    {
        tools.removeFromLeft(gap * 2);
        shiftBtn.setBounds(take(tools, 3.0f));
        multiplyBtn.setBounds(take(tools, 3.5f));
        keepSelectedBtn.setBounds(take(tools, 4.5f));
        keepAnchorsBtn.setBounds(take(tools, 4.5f));
        if (periodBox != nullptr)
            periodBox->setBounds(take(tools, 3.0f));
        if (pageBox != nullptr)
            pageBox->setBounds(take(tools, 2.5f));
    }
    statusArea = editing ? area.removeFromBottom(juce::roundToInt(t.fontSmall + t.spacing * 2.0f)) : juce::Rectangle<int>();

    header->setBounds(area.removeFromTop(juce::roundToInt(t.fontSmall + t.spacing * 2.0f)).withTrimmedRight(viewport.getScrollBarThickness()));
    viewport.setBounds(area);
    table->setSize(viewport.getMaximumVisibleWidth(), static_cast<int>(visible.size()) * rowHeightFor(*this));
}

void TuningView::lookAndFeelChanged()
{
    // Text already in a box keeps the colour it was typed in; a new theme gives it the new one.
    const auto& t = themeOf(*this);
    filterBox.setTextToShowWhenEmpty("Filter: index, Hz or label", t.textDim);
    for (auto* box : {&filterBox, &nameBox})
    {
        box->applyColourToAllText(t.text);
        box->applyFontToAllText(t.font(t.fontNormal));
    }
    repaint();
}

void TuningView::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    g.fillAll(t.surface);
    const auto tuning = shown();
    if (track() < 0)
    {
        g.setColour(t.textDim);
        g.setFont(t.font(t.fontNormal));
        g.drawFittedText("Select a MIDI track to see its tuning.", getLocalBounds().reduced(juce::roundToInt(t.padding * 2.0f)),
                         juce::Justification::centred, 2);
        return;
    }
    if (!draft.isOpen() && tuning != nullptr)
    {
        auto r = titleArea;
        g.setColour(t.text);
        g.setFont(juce::Font(t.font(t.fontNormal)).boldened());
        const auto name = tuning->name;
        const int nameWidth = juce::jmin(r.getWidth(), juce::GlyphArrangement::getStringWidthInt(juce::Font(t.font(t.fontNormal)).boldened(), name) + 8);
        g.drawText(name, r.removeFromLeft(nameWidth), juce::Justification::centredLeft, true);
        g.setColour(t.textDim);
        g.setFont(t.font(t.fontSmall));
        g.drawText(tuning->sourceDescription, r, juce::Justification::centredLeft, true);
    }
    if (draft.isOpen())
    {
        g.setColour(t.textDim);
        g.setFont(t.font(t.fontSmall));
        g.drawText("Editing a copy: the track plays it until you save or discard it.", titleArea.reduced(juce::roundToInt(t.spacing), 0),
                   juce::Justification::centredLeft, true);
        const auto check = problems();
        juce::String status;
        juce::Colour colour = t.textDim;
        for (const auto& p : check.problems)
            if (p.error)
            {
                status = (p.row >= 0 ? "#" + juce::String(p.row) + ": " : juce::String()) + p.message;
                colour = t.error;
                break;
            }
        if (status.isEmpty())
        {
            status = check.aboveLimit > 0 ? juce::String(check.aboveLimit) + " entries are too high to play at "
                                                + juce::String(sampleRate() / 1000.0, 1) + " kHz (muted or folded down)."
                                          : juce::String("Every entry plays.");
            colour = check.aboveLimit > 0 ? t.warning : t.textDim;
        }
        g.setColour(colour);
        g.drawText(status, statusArea, juce::Justification::centredLeft, true);
    }
}

} // namespace lattice::ui
