#include "ui/browser/BrowserView.h"

#include "core/model/LiveSet.h"
#include "devices/presets/Presets.h"
#include "devices/samples/SamplePool.h"
#include "engine/AudioEngine.h"
#include "ui/device/DeviceContext.h"
#include "ui/theme/LatticeLookAndFeel.h"
#include "ui/widgets/Accessible.h"

#include <cmath>
#include <unordered_map>

namespace lattice::ui
{

using library::Item;
using library::Kind;

namespace
{
constexpr const char* previewKey = "browser.preview";
constexpr const char* syncKey = "browser.previewSync";
constexpr const char* volumeKey = "browser.previewVolume";
constexpr const char* sourceKey = "browser.source";

Icon iconFor(const Item& item)
{
    switch (item.kind)
    {
    case Kind::folder:
        return Icon::folder;
    case Kind::sample:
        return Icon::speaker;
    case Kind::device:
    case Kind::preset:
    case Kind::tuning:
        break;
    }
    return Icon::allParams;
}

juce::String sourceId(const BrowserModel::Source& s)
{
    switch (s.kind)
    {
    case BrowserModel::Source::Kind::collection:
        return "collection:" + juce::String(s.collection);
    case BrowserModel::Source::Kind::place:
        return "place:" + s.place.getFullPathName();
    case BrowserModel::Source::Kind::category:
        break;
    }
    return "category:" + s.category;
}
} // namespace

juce::String BrowserView::descriptionFor(const Item& item)
{
    switch (item.kind)
    {
    case Kind::device:
        return drag::device(item.deviceType);
    case Kind::preset:
        return drag::presetKey(item.key);
    case Kind::sample:
        return drag::sample(juce::File(item.key));
    case Kind::tuning:
        return drag::tuning(item.place.isEmpty() ? item.key.fromFirstOccurrenceOf("tuning:", false, false) : item.key);
    case Kind::folder:
        break;
    }
    return {};
}

juce::Colour BrowserView::collectionColour(int collection, const Theme& theme)
{
    // Favourites, orange, yellow, green, blue, purple from the clip palette; grey from the theme.
    static const int paletteIndex[] = {0, 1, 3, 5, 9, 11};
    if (collection >= 0 && collection < 6)
        return juce::Colour(LiveSet::paletteColour(paletteIndex[collection]));
    return theme.textDim;
}

// --- Search box ------------------------------------------------------------------------------------------

class BrowserView::SearchBox final : public juce::TextEditor
{
public:
    std::function<void()> onDown;

    bool keyPressed(const juce::KeyPress& key) override
    {
        if (key == juce::KeyPress::downKey && onDown)
        {
            onDown();
            return true;
        }
        return juce::TextEditor::keyPressed(key);
    }
};

// --- The list ---------------------------------------------------------------------------------------------

/** The rows, drawn a screenful at a time (100,000 results cost what a screenful does). */
class BrowserView::List final : public juce::Component, private juce::ScrollBar::Listener
{
public:
    using Row = BrowserModel::Row;

    explicit List(BrowserView& b) : owner(b)
    {
        setTitle("Browser items");
        accessible::markAsView(*this);
        setWantsKeyboardFocus(true);
        addAndMakeVisible(scroll);
        scroll.setAutoHide(true);
        scroll.addListener(this);
    }

    int rowHeight() const { return juce::roundToInt(themeOf(*this).controlHeight); }
    int first() const { return static_cast<int>(scroll.getCurrentRangeStart()); }

    /** A detail's width in the font: measured once, as the same few ("0.25 s · 48 kHz", "Audio
        Effect") recur down the list and measuring costs as much as drawing. */
    int widthOfDetail(const juce::Font& font, const juce::String& detail)
    {
        if (font != detailFont || detailWidths.size() > 4096)
        {
            detailWidths.clear();
            detailFont = font;
        }
        const auto it = detailWidths.find(detail);
        if (it != detailWidths.end())
            return it->second;
        const int w = juce::GlyphArrangement::getStringWidthInt(font, detail);
        detailWidths.emplace(detail, w);
        return w;
    }

    /** The selected row, for screen readers: "Hit 3, 0.25 s · 48 kHz, 4 of 100". */
    juce::String describeSelection() const
    {
        const auto& rows = owner.browser.rows();
        const int r = owner.browser.selectedRow();
        if (r < 0 || r >= static_cast<int>(rows.size()))
            return rows.empty() ? juce::String("No items") : juce::String(static_cast<int>(rows.size())) + " items, none selected";
        const auto& row = rows[static_cast<std::size_t>(r)];
        auto text = row.item.name;
        if (row.item.detail.isNotEmpty())
            text << ", " << row.item.detail;
        if (row.expandable)
            text << (row.expanded ? ", expanded" : ", collapsed");
        return text << ", " << (r + 1) << " of " << static_cast<int>(rows.size());
    }
    std::unique_ptr<juce::AccessibilityHandler> createAccessibilityHandler() override
    {
        return accessible::viewHandler(*this, juce::AccessibilityRole::list, [this] { return describeSelection(); });
    }
    int fits() const { return juce::jmax(1, getHeight() / juce::jmax(1, rowHeight())); }

    void rowsChanged()
    {
        const auto n = static_cast<double>(owner.browser.rows().size());
        scroll.setRangeLimits(0.0, juce::jmax(n, 1.0));
        scroll.setCurrentRange(scroll.getCurrentRangeStart(), static_cast<double>(fits()));
        repaint();
    }

    void scrollTo(int row)
    {
        if (row < 0)
            return;
        if (row < first())
            scroll.setCurrentRangeStart(row);
        else if (row >= first() + fits())
            scroll.setCurrentRangeStart(row - fits() + 1);
    }

    juce::Rectangle<int> rowArea(int row) const
    {
        return {0, (row - first()) * rowHeight(), getWidth() - (scroll.isVisible() ? scroll.getWidth() : 0), rowHeight()};
    }

    int rowAt(juce::Point<int> p) const
    {
        const int row = first() + p.y / juce::jmax(1, rowHeight());
        return p.y >= 0 && row < static_cast<int>(owner.browser.rows().size()) ? row : -1;
    }

    juce::Rectangle<int> expanderArea(const Row& r, juce::Rectangle<int> area) const
    {
        const auto& t = themeOf(*this);
        const int indent = juce::roundToInt(t.controlHeight * 0.8f);
        return area.withX(area.getX() + juce::roundToInt(t.padding) + r.depth * indent).withWidth(indent);
    }

    void resized() override
    {
        const int w = juce::roundToInt(themeOf(*this).controlHeight * 0.5f);
        scroll.setBounds(getLocalBounds().removeFromRight(w));
        rowsChanged();
    }

    void paint(juce::Graphics& g) override
    {
        const auto& t = themeOf(*this);
        g.fillAll(t.surfaceSunken);
        const auto& rows = owner.browser.rows();
        const int h = rowHeight();
        const int start = first();
        const int end = juce::jmin(static_cast<int>(rows.size()), start + fits() + 1);
        const auto clip = g.getClipBounds();
        for (int r = start; r < end; ++r)
        {
            const auto area = rowArea(r);
            if (!area.intersects(clip))
                continue;
            const auto& row = rows[static_cast<std::size_t>(r)];
            const bool isSelected = r == owner.browser.selectedRow();
            if (isSelected)
            {
                g.setColour(hasKeyboardFocus(false) ? t.selection : t.selection.withMultipliedAlpha(0.6f));
                g.fillRect(area);
            }
            auto content = area.reduced(juce::roundToInt(t.padding), 0);
            const auto expander = expanderArea(row, area);
            content.setLeft(expander.getRight());
            if (row.expandable)
                drawIcon(g, row.expanded ? Icon::foldOpen : Icon::foldClosed, expander.toFloat().reduced(h * 0.16f), t.textDim, t);
            const auto icon = content.removeFromLeft(h);
            drawIcon(g, iconFor(row.item), icon.toFloat().reduced(h * 0.28f), row.item.kind == Kind::folder ? t.textDim : t.accent, t);
            content.removeFromLeft(juce::roundToInt(t.spacing));

            // Collection dots on the right, then the detail, then the name gets what's left.
            for (int c = library::LibraryIndex::numCollections - 1; c >= 0; --c)
                if (row.item.inCollection(c))
                {
                    const auto dot = content.removeFromRight(juce::roundToInt(h * 0.5f)).toFloat().withSizeKeepingCentre(h * 0.3f, h * 0.3f);
                    g.setColour(collectionColour(c, t));
                    g.fillEllipse(dot);
                }
            g.setFont(juce::Font(t.font(t.fontSmall)));
            const auto detailWidth = juce::jmin(content.getWidth() / 2, widthOfDetail(g.getCurrentFont(), row.item.detail) + juce::roundToInt(t.spacing));
            const auto detail = content.removeFromRight(detailWidth);
            g.setColour(isSelected ? t.text : t.textDim);
            g.drawText(row.item.detail, detail, juce::Justification::centredRight, true);
            g.setFont(juce::Font(t.font(t.fontNormal)));
            g.setColour(t.text);
            g.drawText(row.item.name, content, juce::Justification::centredLeft, true);
        }
        if (rows.empty())
        {
            g.setColour(t.textDim);
            g.setFont(juce::Font(t.font(t.fontNormal)));
            const auto text = owner.browser.loading() ? juce::String("Looking...")
                              : owner.browser.searching() ? "Nothing matches \"" + owner.browser.search().trim() + "\"."
                                                          : juce::String("Nothing here yet.");
            g.drawFittedText(text, getLocalBounds().reduced(juce::roundToInt(t.padding * 2.0f)), juce::Justification::centredTop, 3);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        grabKeyboardFocus();
        dragging = false;
        const int row = rowAt(e.getPosition());
        pressedRow = row;
        if (row < 0)
            return;
        const auto& r = owner.browser.rows()[static_cast<std::size_t>(row)];
        if (e.mods.isPopupMenu())
        {
            owner.browser.select(row);
            showMenu(row);
            return;
        }
        if (r.expandable && expanderArea(r, rowArea(row)).contains(e.getPosition()))
        {
            owner.browser.setExpanded(row, !r.expanded);
            return;
        }
        owner.browser.select(row);
    }

    void mouseDrag(const juce::MouseEvent& e) override
    {
        if (dragging || pressedRow < 0 || e.getDistanceFromDragStart() < 4 || e.mods.isPopupMenu())
            return;
        const auto& rows = owner.browser.rows();
        if (pressedRow >= static_cast<int>(rows.size()))
            return;
        const auto& item = rows[static_cast<std::size_t>(pressedRow)].item;
        const auto description = descriptionFor(item);
        auto* container = juce::DragAndDropContainer::findParentDragContainerFor(this);
        if (description.isEmpty() || container == nullptr)
            return;
        dragging = true;
        // The row's name, as what's dragged.
        const auto& t = themeOf(*this);
        juce::Font font(t.font(t.fontNormal));
        const int w = juce::GlyphArrangement::getStringWidthInt(font, item.name) + rowHeight();
        juce::Image image(juce::Image::ARGB, juce::jmax(1, w), rowHeight(), true);
        {
            juce::Graphics g(image);
            g.setColour(t.surfaceRaised.withAlpha(0.9f));
            g.fillRoundedRectangle(image.getBounds().toFloat(), t.radius);
            g.setColour(t.text);
            g.setFont(font);
            g.drawText(item.name, image.getBounds().reduced(rowHeight() / 2, 0), juce::Justification::centredLeft, true);
        }
        container->startDragging(description, this, juce::ScaledImage(image), false);
    }

    void mouseDoubleClick(const juce::MouseEvent& e) override
    {
        const int row = rowAt(e.getPosition());
        if (row < 0 || e.mods.isPopupMenu())
            return;
        owner.browser.select(row);
        owner.insertSelected();
    }

    void mouseWheelMove(const juce::MouseEvent&, const juce::MouseWheelDetails& wheel) override
    {
        scroll.setCurrentRangeStart(scroll.getCurrentRangeStart() - wheel.deltaY * 10.0);
    }

    bool keyPressed(const juce::KeyPress& key) override
    {
        auto& m = owner.browser;
        const int row = m.selectedRow();
        const int n = static_cast<int>(m.rows().size());
        if (n == 0)
            return false;
        const auto move = [&](int to) {
            m.select(juce::jlimit(0, n - 1, to));
            scrollTo(m.selectedRow());
            return true;
        };
        if (key == juce::KeyPress::downKey)
            return move(row + 1);
        if (key == juce::KeyPress::upKey)
        {
            if (row <= 0)
            {
                owner.focusSearch(); // Up from the top goes back to the search
                return true;
            }
            return move(row - 1);
        }
        if (key == juce::KeyPress::pageDownKey)
            return move(row + fits());
        if (key == juce::KeyPress::pageUpKey)
            return move(row - fits());
        if (key == juce::KeyPress::homeKey)
            return move(0);
        if (key == juce::KeyPress::endKey)
            return move(n - 1);
        if (key == juce::KeyPress::rightKey)
        {
            m.setExpanded(row, true);
            return true;
        }
        if (key == juce::KeyPress::leftKey)
        {
            m.collapseOrParent(row);
            scrollTo(m.selectedRow());
            return true;
        }
        if (key == juce::KeyPress::returnKey)
        {
            owner.insertSelected();
            return true;
        }
        return false; // letters go on to the computer keyboard
    }

    void focusGained(FocusChangeType) override { repaint(); }
    void focusLost(FocusChangeType) override { repaint(); }

private:
    void scrollBarMoved(juce::ScrollBar*, double) override { repaint(); }

    void showMenu(int row)
    {
        const auto item = owner.browser.rows()[static_cast<std::size_t>(row)].item;
        juce::PopupMenu menu;
        const auto description = descriptionFor(item);
        menu.addItem("Insert", description.isNotEmpty(), false, [safe = juce::Component::SafePointer<List>(this)] {
            if (safe != nullptr)
                safe->owner.insertSelected();
        });
        if (item.kind == Kind::sample || item.kind == Kind::preset)
            menu.addItem("Preview", [safe = juce::Component::SafePointer<List>(this)] {
                if (safe != nullptr)
                {
                    safe->owner.setPreviewOn(true);
                    safe->owner.previewSelected();
                }
            });
        juce::PopupMenu collections;
        for (int c = 0; c < library::LibraryIndex::numCollections; ++c)
        {
            const bool in = item.inCollection(c);
            collections.addItem(library::LibraryIndex::collectionName(c), true, in,
                                [lib = &owner.lib, key = item.key, c, in] { lib->index().setInCollection(key, c, !in); });
        }
        menu.addSubMenu("Collections", collections);
        if (item.place.isNotEmpty())
            menu.addItem(juce::String("Show in ") + (juce::SystemStats::getOperatingSystemType() & juce::SystemStats::Windows
                                                          ? "Explorer"
                                                          : "Files"),
                         [file = juce::File(item.key)] { file.revealToUser(); });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
    }

    BrowserView& owner;
    juce::ScrollBar scroll{true};
    std::unordered_map<juce::String, int> detailWidths;
    juce::Font detailFont{juce::FontOptions{}};
    int pressedRow = -1;
    bool dragging = false;
};

// --- The sidebar ------------------------------------------------------------------------------------------

class BrowserView::Sidebar final : public juce::Component
{
public:
    explicit Sidebar(BrowserView& b) : owner(b) { setTitle("Browser sources"); }

    struct Entry
    {
        enum class Type
        {
            heading,
            source,
            addPlace
        };
        Type type = Type::heading;
        juce::String text;
        BrowserModel::Source source;
    };

    void rebuild()
    {
        entries.clear();
        const auto all = owner.browser.sources();
        BrowserModel::Source::Kind last = BrowserModel::Source::Kind::collection;
        bool first = true;
        for (const auto& s : all)
        {
            if (first || s.kind != last)
            {
                const auto heading = s.kind == BrowserModel::Source::Kind::collection ? "Collections"
                                     : s.kind == BrowserModel::Source::Kind::category ? "Categories"
                                                                                      : "Places";
                entries.push_back({Entry::Type::heading, heading, {}});
                last = s.kind;
                first = false;
            }
            entries.push_back({Entry::Type::source, s.name, s});
        }
        entries.push_back({Entry::Type::addPlace, "Add Folder...", {}});
        setSize(getWidth(), rowHeight() * static_cast<int>(entries.size()) + rowHeight() / 2);
        repaint();
    }

    int rowHeight() const { return juce::roundToInt(themeOf(*this).controlHeight); }

    juce::Rectangle<int> boundsOf(const BrowserModel::Source& s) const
    {
        for (std::size_t i = 0; i < entries.size(); ++i)
            if (entries[i].type == Entry::Type::source && entries[i].source == s)
                return {0, static_cast<int>(i) * rowHeight(), getWidth(), rowHeight()};
        return {};
    }

    void paint(juce::Graphics& g) override
    {
        const auto& t = themeOf(*this);
        g.fillAll(t.surface);
        const int h = rowHeight();
        const auto& shown = owner.browser.source();
        const bool searching = owner.browser.searching();
        for (std::size_t i = 0; i < entries.size(); ++i)
        {
            const auto& e = entries[i];
            auto area = juce::Rectangle<int>(0, static_cast<int>(i) * h, getWidth(), h).reduced(juce::roundToInt(t.padding), 0);
            if (e.type == Entry::Type::heading)
            {
                g.setColour(t.textDim);
                g.setFont(juce::Font(t.font(t.fontSmall)).boldened());
                g.drawText(e.text.toUpperCase(), area, juce::Justification::bottomLeft, true);
                continue;
            }
            const bool isShown = e.type == Entry::Type::source && e.source == shown && !searching;
            if (isShown)
            {
                g.setColour(t.selection);
                g.fillRoundedRectangle(area.toFloat(), t.radius);
            }
            auto content = area.reduced(juce::roundToInt(t.spacing), 0);
            const auto mark = content.removeFromLeft(h).toFloat();
            if (e.type == Entry::Type::addPlace)
                drawIcon(g, Icon::plus, mark.reduced(h * 0.3f), t.textDim, t);
            else if (e.source.kind == BrowserModel::Source::Kind::collection)
            {
                g.setColour(collectionColour(e.source.collection, t));
                g.fillEllipse(mark.withSizeKeepingCentre(h * 0.32f, h * 0.32f));
            }
            else if (e.source.kind == BrowserModel::Source::Kind::place)
                drawIcon(g, Icon::folder, mark.reduced(h * 0.26f), t.textDim, t);
            g.setColour(e.type == Entry::Type::addPlace ? t.textDim : t.text);
            g.setFont(juce::Font(t.font(t.fontNormal)));
            g.drawText(e.text, content, juce::Justification::centredLeft, true);
        }
    }

    void mouseDown(const juce::MouseEvent& e) override
    {
        const auto i = static_cast<std::size_t>(e.y / juce::jmax(1, rowHeight()));
        if (e.y < 0 || i >= entries.size())
            return;
        const auto entry = entries[i];
        if (entry.type == Entry::Type::addPlace)
        {
            owner.addPlace();
            return;
        }
        if (entry.type != Entry::Type::source)
            return;
        if (e.mods.isPopupMenu() && entry.source.kind == BrowserModel::Source::Kind::place)
        {
            showPlaceMenu(entry.source.place);
            return;
        }
        owner.search->clear();
        owner.browser.setSource(entry.source);
        owner.saveSettings();
        repaint();
    }

private:
    void showPlaceMenu(const juce::File& place)
    {
        juce::PopupMenu menu;
        auto* library = &owner.lib;
        menu.addItem("Rescan", [library] { library->rescanAll(); });
        menu.addItem(juce::String("Show in ") + (juce::SystemStats::getOperatingSystemType() & juce::SystemStats::Windows ? "Explorer" : "Files"),
                     [place] { place.revealToUser(); });
        menu.addItem("Remove from Places", place != owner.lib.userLibrary(), false, [library, place] { library->removePlace(place); });
        menu.showMenuAsync(juce::PopupMenu::Options().withTargetComponent(this).withMousePosition());
    }

    BrowserView& owner;
    std::vector<Entry> entries;
};

// --- The preview strip ------------------------------------------------------------------------------------

/** Preview on/off, synced to the bar or not, its volume, and the selected sample's waveform with
    where the preview is. */
class BrowserView::PreviewStrip final : public juce::Component
{
public:
    explicit PreviewStrip(BrowserView& b) : owner(b)
    {
        setTitle("Preview");
        for (auto* button : {&preview, &sync})
        {
            button->setClickingTogglesState(true);
            addAndMakeVisible(*button);
        }
        preview.onClick = [this] { owner.setPreviewOn(preview.getToggleState()); };
        sync.onClick = [this] { owner.setPreviewSynced(sync.getToggleState()); };
        volume.setSliderStyle(juce::Slider::LinearBar);
        volume.setTextBoxStyle(juce::Slider::NoTextBox, false, 0, 0);
        volume.setRange(0.0, 1.0);
        volume.setTitle("Preview volume");
        volume.setTooltip("The preview's volume.");
        volume.setWantsKeyboardFocus(false);
        volume.onValueChange = [this] {
            owner.engine.preview().setGain(static_cast<float>(volume.getValue()));
            owner.saveSettings();
        };
        addAndMakeVisible(volume);
    }

    void setPeaks(std::shared_ptr<const library::WaveformPeaks> p)
    {
        peaks = std::move(p);
        image = {};
        playhead = -1;
        repaint();
    }
    bool hasPeaks() const noexcept { return peaks != nullptr; }
    void setSoundGeneration(std::uint64_t g) { soundGeneration = g; }

    void resized() override
    {
        const auto& t = themeOf(*this);
        auto area = getLocalBounds().reduced(juce::roundToInt(t.padding));
        auto row = area.removeFromTop(juce::roundToInt(t.controlHeight));
        preview.setBounds(row.removeFromLeft(row.getHeight()));
        row.removeFromLeft(juce::roundToInt(t.spacing));
        sync.setBounds(row.removeFromLeft(row.getHeight()));
        row.removeFromLeft(juce::roundToInt(t.spacing));
        volume.setBounds(row.removeFromLeft(juce::roundToInt(t.controlHeight * 4.0f)));
        area.removeFromTop(juce::roundToInt(t.spacing));
        wave = area;
        image = {};
    }

    void paint(juce::Graphics& g) override
    {
        const auto& t = themeOf(*this);
        g.fillAll(t.surface);
        g.setColour(t.surfaceSunken);
        g.fillRoundedRectangle(wave.toFloat(), t.radius);
        if (peaks == nullptr || wave.isEmpty())
        {
            g.setColour(t.textDim);
            g.setFont(juce::Font(t.font(t.fontSmall)));
            g.drawFittedText(owner.previewing ? "Select a sample or an instrument preset to hear it." : "Preview is off.",
                             wave.reduced(juce::roundToInt(t.padding)), juce::Justification::centred, 2);
            return;
        }
        if (!image.isValid())
            drawWave(t);
        g.drawImageAt(image, wave.getX(), wave.getY());
        if (playhead >= 0)
        {
            g.setColour(t.text);
            g.fillRect(juce::Rectangle<float>(static_cast<float>(wave.getX() + playhead), static_cast<float>(wave.getY()),
                                              juce::jmax(1.0f, t.stroke), static_cast<float>(wave.getHeight())));
        }
    }

    /** The preview moved: only the playhead's old and new places are drawn again. */
    void previewMoved(std::int64_t frame, std::uint64_t generation)
    {
        int x = -1;
        if (peaks != nullptr && peaks->length > 0 && frame >= 0 && generation == soundGeneration)
            x = static_cast<int>(static_cast<double>(frame) / static_cast<double>(peaks->length) * wave.getWidth());
        if (x == playhead)
            return;
        const int w = juce::jmax(2, juce::roundToInt(themeOf(*this).stroke) + 2);
        if (playhead >= 0)
            repaint(wave.getX() + playhead - 1, wave.getY(), w, wave.getHeight());
        playhead = x;
        if (playhead >= 0)
            repaint(wave.getX() + playhead - 1, wave.getY(), w, wave.getHeight());
    }

    void sync_(bool previewOn, bool syncOn, float gain)
    {
        preview.setToggleState(previewOn, juce::dontSendNotification);
        sync.setToggleState(syncOn, juce::dontSendNotification);
        volume.setValue(gain, juce::dontSendNotification);
    }

    int playheadX() const noexcept { return playhead; }

private:
    void drawWave(const Theme& t)
    {
        image = juce::Image(juce::Image::ARGB, juce::jmax(1, wave.getWidth()), juce::jmax(1, wave.getHeight()), true);
        juce::Graphics g(image);
        g.setColour(t.accent.withMultipliedAlpha(0.8f));
        const float mid = wave.getHeight() * 0.5f;
        const double perPixel = static_cast<double>(peaks->length) / juce::jmax(1, wave.getWidth());
        for (int x = 0; x < wave.getWidth(); ++x)
        {
            const auto [lo, hi] = peaks->range(x * perPixel, (x + 1) * perPixel);
            const float top = mid - hi * mid, bottom = mid - lo * mid;
            g.fillRect(juce::Rectangle<float>(static_cast<float>(x), top, 1.0f, juce::jmax(1.0f, bottom - top)));
        }
    }

    BrowserView& owner;
    IconButton preview{"Preview", Icon::speaker, "Preview: selecting a sample plays it, and an instrument preset plays a phrase."};
    IconButton sync{"Sync", Icon::sync, "Synced: while the set plays, a preview starts on the next bar."};
    juce::Slider volume;
    juce::Rectangle<int> wave;
    std::shared_ptr<const library::WaveformPeaks> peaks;
    std::uint64_t soundGeneration = 0;
    juce::Image image;
    int playhead = -1;
};

// --- BrowserView ------------------------------------------------------------------------------------------

BrowserView::BrowserView(library::Library& l, JobPool& j, AudioEngine& e, EngineStateFeed& f, presets::PresetLibrary& p,
                         juce::PropertiesFile* s, Actions a)
    : lib(l), jobs(j), engine(e), feed(f), presetLibrary(p), settings(s), actions(std::move(a)), browser(l, j)
{
    setTitle("Browser");
    search = std::make_unique<SearchBox>();
    search->setTitle("Search the browser");
    search->setJustification(juce::Justification::centredLeft);
    search->onTextChange = [this] { browser.setSearch(search->getText()); };
    search->onEscapeKey = [this] {
        search->clear();
        browser.setSearch({});
        focusList();
    };
    search->onReturnKey = [this] {
        if (browser.selectedRow() < 0 && !browser.rows().empty())
            browser.select(0);
        focusList();
    };
    search->onDown = [this] {
        if (browser.selectedRow() < 0 && !browser.rows().empty())
            browser.select(0);
        focusList();
    };
    addAndMakeVisible(*search);

    sidebar = std::make_unique<Sidebar>(*this);
    sidebarView = std::make_unique<juce::Viewport>();
    sidebarView->setViewedComponent(sidebar.get(), false);
    sidebarView->setScrollBarsShown(true, false);
    sidebarView->setWantsKeyboardFocus(false);
    addAndMakeVisible(*sidebarView);

    list = std::make_unique<List>(*this);
    addAndMakeVisible(*list);
    strip = std::make_unique<PreviewStrip>(*this);
    addAndMakeVisible(*strip);

    if (settings != nullptr)
    {
        previewing = settings->getBoolValue(previewKey, true);
        synced = settings->getBoolValue(syncKey, false);
        engine.preview().setGain(static_cast<float>(settings->getDoubleValue(volumeKey, 0.7)));
        const auto saved = settings->getValue(sourceKey, {});
        for (const auto& source : browser.sources())
            if (sourceId(source) == saved)
                browser.setSource(source);
    }
    strip->sync_(previewing, synced, engine.preview().getGain());

    browser.onRowsChanged = [this] { rowsChanged(); };
    browser.onSelectionChanged = [this] { selectionChanged(); };
    feed.addListener(this, EngineStateFeed::previewAspect);
    sidebar->rebuild();
    lookAndFeelChanged();
}

BrowserView::~BrowserView()
{
    feed.removeListener(this);
    if (peaksJob != 0)
        jobs.cancel(peaksJob);
    engine.preview().stop();
}

juce::TextEditor& BrowserView::searchBox()
{
    return *search;
}

void BrowserView::focusSearch()
{
    if (isShowing())
    {
        search->grabKeyboardFocus();
        search->selectAll();
    }
}

void BrowserView::focusList()
{
    if (isShowing())
        list->grabKeyboardFocus();
}

void BrowserView::setPreviewOn(bool on)
{
    previewing = on;
    strip->sync_(previewing, synced, engine.preview().getGain());
    if (!on)
        engine.preview().stop();
    saveSettings();
    strip->repaint();
}

void BrowserView::setPreviewSynced(bool on)
{
    synced = on;
    strip->sync_(previewing, synced, engine.preview().getGain());
    saveSettings();
}

void BrowserView::saveSettings()
{
    if (settings == nullptr)
        return;
    settings->setValue(previewKey, previewing);
    settings->setValue(syncKey, synced);
    settings->setValue(volumeKey, static_cast<double>(engine.preview().getGain()));
    settings->setValue(sourceKey, sourceId(browser.source()));
}

void BrowserView::addPlace()
{
    if (!actions.chooseFolder)
        return;
    juce::Component::SafePointer<BrowserView> safe(this);
    actions.chooseFolder([safe](const juce::File& folder) {
        if (safe == nullptr || !folder.isDirectory())
            return;
        safe->lib.addPlace(folder);
        safe->sidebar->rebuild();
        for (const auto& s : safe->browser.sources())
            if (s.kind == BrowserModel::Source::Kind::place && s.place == folder)
                safe->browser.setSource(s);
        safe->saveSettings();
    });
}

bool BrowserView::insertSelected()
{
    const auto* row = browser.selectedItem();
    if (row == nullptr)
        return false;
    if (row->item.kind == Kind::folder)
    {
        browser.setExpanded(browser.selectedRow(), !row->expanded);
        return true;
    }
    const auto description = descriptionFor(row->item);
    return description.isNotEmpty() && actions.insert && actions.insert(description);
}

void BrowserView::previewSelected()
{
    const auto generation = ++previewRequest;
    if (peaksJob != 0)
        jobs.cancel(peaksJob);
    peaksJob = 0;
    strip->setPeaks(nullptr);
    strip->setSoundGeneration(0);
    const auto* row = browser.selectedItem();
    if (!previewing || row == nullptr)
    {
        engine.preview().stop();
        return;
    }
    const auto item = row->item;
    juce::Component::SafePointer<BrowserView> safe(this);
    if (item.kind == Kind::sample)
    {
        const juce::File file(item.key);
        // The waveform (from the disk cache, or read and stored) and the sound, both off this thread.
        auto peaks = std::make_shared<std::shared_ptr<const library::WaveformPeaks>>();
        const auto* cache = &lib.peaks();
        peaksJob = jobs.submit("Waveform of " + item.name, [cache, file, peaks](JobPool::Context& work) {
            *peaks = cache->get(file, [&work] { return work.isCancelled(); });
        },
                               [safe, peaks, generation](bool cancelled) {
                                   if (safe == nullptr || cancelled || generation != safe->previewRequest)
                                       return;
                                   safe->peaksJob = 0;
                                   safe->strip->setPeaks(*peaks);
                               });
        SamplePool::shared().request(file, [safe, generation](SamplePtr sample, const juce::String&) {
            if (safe == nullptr || generation != safe->previewRequest || !safe->previewing || sample == nullptr)
                return;
            // The strip's playhead follows this sound.
            safe->strip->setSoundGeneration(safe->engine.preview().play(std::move(sample), safe->synced));
            safe->engine.noteActivity(); // the feed shows the playhead from the next frame
        });
        return;
    }
    if (item.kind == Kind::preset && lib.presetPreviews().plays(item.deviceType))
    {
        const auto play = [safe, generation, key = item.key](const juce::ValueTree& device) {
            if (safe == nullptr || generation != safe->previewRequest || !device.isValid())
                return;
            const double rate = safe->actions.sampleRate ? safe->actions.sampleRate() : 48000.0;
            safe->lib.presetPreviews().request(key, device, rate, [safe, generation](SamplePtr phrase) {
                if (safe == nullptr || generation != safe->previewRequest || !safe->previewing || phrase == nullptr)
                    return;
                safe->engine.preview().play(std::move(phrase), safe->synced);
                safe->engine.noteActivity();
            });
        };
        if (const auto* info = presetLibrary.find(item.key); info != nullptr && info->factory)
        {
            presets::Preset p;
            if (presetLibrary.load(*info, p).wasOk()) // a factory preset is in memory
                play(p.device);
            return;
        }
        // A file: read off this thread.
        auto read = std::make_shared<presets::Preset>();
        jobs.submit("Reading " + item.name, [file = juce::File(item.key), read](JobPool::Context&) { presets::read(file, *read); },
                    [read, play](bool cancelled) {
                        if (!cancelled)
                            play(read->device);
                    });
        return;
    }
    engine.preview().stop();
}

void BrowserView::rowsChanged()
{
    sidebar->rebuild(); // places may have come or gone
    list->rowsChanged();
    if (browser.selectedRow() >= 0)
        list->scrollTo(browser.selectedRow());
    if (onBrowserChanged)
        onBrowserChanged();
}

void BrowserView::selectionChanged()
{
    list->repaint();
    if (browser.selectedRow() >= 0)
        list->scrollTo(browser.selectedRow()); // a controller may have moved it
    accessible::selectionChanged(*list);
    previewSelected();
    if (onBrowserChanged)
        onBrowserChanged();
}

void BrowserView::engineStateChanged(std::uint32_t aspects)
{
    if ((aspects & EngineStateFeed::previewAspect) != 0)
        strip->previewMoved(feed.previewFrame(), feed.previewGeneration());
}

int BrowserView::firstVisibleRow() const
{
    return list->first();
}

int BrowserView::visibleRows() const
{
    return list->fits();
}

juce::Component& BrowserView::listComponent() noexcept
{
    return *list;
}

void BrowserView::scrollToRow(int row)
{
    list->scrollTo(row);
}

juce::Rectangle<int> BrowserView::listBounds() const
{
    return list->getBounds();
}

juce::Rectangle<int> BrowserView::rowBounds(int row) const
{
    return list->rowArea(row) + list->getPosition();
}

juce::Rectangle<int> BrowserView::sourceBounds(const BrowserModel::Source& source) const
{
    return sidebar->boundsOf(source) + sidebarView->getPosition() - sidebarView->getViewPosition();
}

bool BrowserView::showsWaveform() const
{
    return strip->hasPeaks();
}

void BrowserView::lookAndFeelChanged()
{
    const auto& t = themeOf(*this);
    search->setFont(juce::Font(t.font(t.fontNormal)));
    search->setIndents(juce::roundToInt(t.padding * 2.0f), 0);
    search->setTextToShowWhenEmpty("Search (Ctrl+Alt+F)", t.textDim);
    search->applyFontToAllText(juce::Font(t.font(t.fontNormal)));
    resized();
    repaint();
}

void BrowserView::resized()
{
    const auto& t = themeOf(*this);
    auto area = getLocalBounds();
    const int gap = juce::roundToInt(t.spacing);
    search->setBounds(area.removeFromTop(juce::roundToInt(t.controlHeight * 1.2f)).reduced(gap));
    strip->setBounds(area.removeFromBottom(juce::roundToInt(t.controlHeight * 3.2f)));
    // The sources take about a third, never less than their names need.
    const int side = juce::jlimit(juce::roundToInt(t.controlHeight * 5.0f), juce::jmax(juce::roundToInt(t.controlHeight * 5.0f), area.getWidth() / 2),
                                  area.getWidth() * 2 / 5);
    sidebarView->setBounds(area.removeFromLeft(side));
    sidebar->setSize(sidebarView->getMaximumVisibleWidth(), sidebar->getHeight());
    sidebar->rebuild();
    list->setBounds(area);
}

void BrowserView::paint(juce::Graphics& g)
{
    g.fillAll(themeOf(*this).surface);
}

} // namespace lattice::ui
