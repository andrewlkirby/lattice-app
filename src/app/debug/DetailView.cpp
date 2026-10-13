#include "app/debug/DetailView.h"

#include "platform/Platform.h"

#include <map>

namespace lattice::debugui
{

namespace
{
constexpr auto sampleFoldersKey = "sampleFolders";
constexpr int browserWidth = 210;
} // namespace

// --- Device list -------------------------------------------------------------------------------------------

/** A heading or a leaf with a drag description. */
class DeviceBrowser::Item : public juce::TreeViewItem
{
public:
    Item(juce::String text, juce::String description, std::function<void(const juce::String&)>* insert)
        : label(std::move(text)), drag(std::move(description)), onInsert(insert)
    {
    }

    bool mightContainSubItems() override { return drag.isEmpty(); }
    bool canBeSelected() const override { return drag.isNotEmpty(); }
    juce::String getUniqueName() const override { return label; }
    int getItemHeight() const override { return 20; }

    void paintItem(juce::Graphics& g, int width, int height) override
    {
        if (isSelected())
            g.fillAll(colours::emptySlot.brighter(0.2f));
        g.setColour(drag.isEmpty() ? colours::dimText : colours::text);
        g.setFont(juce::FontOptions(12.0f, drag.isEmpty() ? juce::Font::bold : juce::Font::plain));
        g.drawText(label, 4, 0, width - 4, height, juce::Justification::centredLeft, true);
    }

    juce::var getDragSourceDescription() override { return drag; }

    void itemDoubleClicked(const juce::MouseEvent&) override
    {
        if (drag.isNotEmpty() && onInsert != nullptr && *onInsert)
            (*onInsert)(drag);
        else
            setOpen(!isOpen());
    }

protected:
    juce::String label;
    juce::String drag;
    std::function<void(const juce::String&)>* onInsert;
};

/** A sample folder: its sub-folders and audio files, read when opened. */
class DeviceBrowser::FolderItem final : public Item
{
public:
    FolderItem(juce::File f, std::function<void(const juce::String&)>* insert) : Item(f.getFileName(), {}, insert), folder(std::move(f)) {}

    juce::String getUniqueName() const override { return folder.getFullPathName(); }

    void itemOpennessChanged(bool isNowOpen) override
    {
        if (!isNowOpen || getNumSubItems() > 0)
            return;
        auto entries = folder.findChildFiles(juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles, false);
        entries.sort();
        for (const auto& e : entries)
            if (e.isDirectory())
                addSubItem(new FolderItem(e, onInsert));
        for (const auto& e : entries)
            if (e.existsAsFile() && isSampleFile(e))
                addSubItem(new Item(e.getFileName(), drag::sample(e), onInsert));
    }

private:
    juce::File folder;
};

DeviceBrowser::DeviceBrowser(EditorContext& c, juce::PropertiesFile& s) : context(c), settings(s)
{
    tree.setRootItemVisible(false);
    tree.setDefaultOpenness(false);
    tree.setColour(juce::TreeView::backgroundColourId, colours::panel);
    tree.setWantsKeyboardFocus(false);
    addAndMakeVisible(tree);
    addFolder.setTooltip("Add a sample folder to the list");
    addFolder.setWantsKeyboardFocus(false);
    addFolder.onClick = [this] { addSampleFolder(); };
    addAndMakeVisible(addFolder);
    refresh();
}

DeviceBrowser::~DeviceBrowser()
{
    tree.setRootItem(nullptr);
}

juce::StringArray DeviceBrowser::sampleFolders() const
{
    return juce::StringArray::fromLines(settings.getValue(sampleFoldersKey));
}

void DeviceBrowser::refresh()
{
    const auto openness = tree.getOpennessState(true);
    tree.setRootItem(nullptr);
    root = std::make_unique<Item>("root", juce::String(), &onInsert);

    // Devices by category, in the registry's order.
    std::map<juce::String, Item*> categories;
    for (const auto& type : context.registry().types())
    {
        if (type.hidden)
            continue;
        auto*& heading = categories[type.category];
        if (heading == nullptr)
        {
            heading = new Item(type.category, {}, &onInsert);
            root->addSubItem(heading);
        }
        heading->addSubItem(new Item(type.name, drag::device(type.id), &onInsert));
    }

    // Presets by device.
    auto* presetsHeading = new Item("Presets", {}, &onInsert);
    root->addSubItem(presetsHeading);
    std::map<juce::String, Item*> byDevice;
    const auto& all = context.presets.all();
    for (std::size_t i = 0; i < all.size(); ++i)
    {
        const auto* info = context.registry().find(all[i].deviceType);
        const auto deviceName = info != nullptr ? info->name : all[i].deviceType;
        auto*& heading = byDevice[deviceName];
        if (heading == nullptr)
        {
            heading = new Item(deviceName, {}, &onInsert);
            presetsHeading->addSubItem(heading);
        }
        heading->addSubItem(new Item(all[i].name + (all[i].factory ? "" : "  (user)"),
                                     drag::preset(static_cast<int>(i)), &onInsert));
    }

    // Sample folders.
    auto* samples = new Item("Samples", {}, &onInsert);
    root->addSubItem(samples);
    for (const auto& path : sampleFolders())
        if (juce::File(path).isDirectory())
            samples->addSubItem(new FolderItem(juce::File(path), &onInsert));

    tree.setRootItem(root.get());
    if (openness != nullptr)
        tree.restoreOpennessState(*openness, true);
}

void DeviceBrowser::addSampleFolder()
{
    chooser = std::make_unique<juce::FileChooser>("Add a sample folder",
                                                  juce::File::getSpecialLocation(juce::File::userMusicDirectory));
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                         [this](const juce::FileChooser& fc) {
                             const auto folder = fc.getResult();
                             if (!folder.isDirectory())
                                 return;
                             auto folders = sampleFolders();
                             folders.addIfNotAlreadyThere(folder.getFullPathName());
                             settings.setValue(sampleFoldersKey, folders.joinIntoString("\n"));
                             refresh();
                         });
}

void DeviceBrowser::paint(juce::Graphics& g)
{
    g.fillAll(colours::panel);
}

void DeviceBrowser::resized()
{
    auto area = getLocalBounds();
    addFolder.setBounds(area.removeFromBottom(24).reduced(2));
    tree.setBounds(area);
}

// --- Detail view ------------------------------------------------------------------------------------------

DetailView::DetailView(SessionController& s, AudioEngine& engine, juce::PropertiesFile& settings)
    : session(s), specs(s.deviceRegistry()),
      library(presets::PresetLibrary::defaultUserRoot(platform::defaultPaths().userData), s.deviceRegistry()),
      context{s, engine, specs, library,
              [this](const juce::String& title, const juce::String& patterns, std::function<void(const juce::File&)> chosen) {
                  chooseFile(title, patterns, std::move(chosen));
              },
              [this] { browser.refresh(); }},
      browser(context, settings)
{
    browser.onInsert = [this](const juce::String& description) {
        if (row != nullptr)
            row->insertFromDescription(description, -1);
    };
    addAndMakeVisible(browser);
    rowView.setScrollBarsShown(false, true);
    rowView.setWantsKeyboardFocus(false);
    addAndMakeVisible(rowView);
    session.set().state().addListener(this);
    rebuildRow();
}

DetailView::~DetailView()
{
    cancelPendingUpdate();
    session.set().state().removeListener(this);
}

void DetailView::flushUpdates()
{
    handleUpdateNowIfNeeded();
    if (row != nullptr)
        row->flushUpdates();
}

void DetailView::handleAsyncUpdate()
{
    // The shown track or return went away, or another one now sits at its index.
    if (row == nullptr || !shownChain.isAChildOf(session.set().state()) || row->chain() != LiveSet::chainOf(ownerNode()))
        rebuildRow();
}

juce::ValueTree DetailView::ownerNode() const
{
    auto& set = session.set();
    switch (shown.kind)
    {
    case ChainOwner::Kind::Track:
        return set.track(shown.index);
    case ChainOwner::Kind::Return:
        return set.returnTrack(shown.index);
    case ChainOwner::Kind::Master:
        return set.master();
    }
    return {};
}

juce::String DetailView::ownerName() const
{
    const auto node = ownerNode();
    switch (shown.kind)
    {
    case ChainOwner::Kind::Track:
        return node[ids::name].toString();
    case ChainOwner::Kind::Return:
        return node[ids::name].toString();
    case ChainOwner::Kind::Master:
        return "Master";
    }
    return {};
}

void DetailView::showChainOf(ChainOwner owner)
{
    if (owner == shown && row != nullptr && row->chain() == LiveSet::chainOf(ownerNode()))
        return;
    shown = owner;
    rebuildRow();
}

void DetailView::rebuildRow()
{
    rowView.setViewedComponent(nullptr, false);
    row.reset();
    const auto node = ownerNode();
    if (!node.isValid())
    {
        // The track or return went away: fall back to the first track.
        if (shown.kind != ChainOwner::Kind::Track || shown.index != 0)
        {
            shown = {};
            rebuildRow();
        }
        return;
    }
    shownChain = LiveSet::chainOf(node);
    if (!shownChain.isValid())
        return;
    row = std::make_unique<ChainRow>(context, shownChain, std::max(120, rowView.getHeight() - rowView.getScrollBarThickness()));
    rowView.setViewedComponent(row.get(), false);
    row->onWidthChanged = [this] { resized(); };
    resized();
    repaint();
}

void DetailView::chooseFile(const juce::String& title, const juce::String& patterns,
                            std::function<void(const juce::File&)> chosen)
{
    chooser = std::make_unique<juce::FileChooser>(title, juce::File::getSpecialLocation(juce::File::userMusicDirectory),
                                                  patterns);
    chooser->launchAsync(juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                         [chosen = std::move(chosen)](const juce::FileChooser& fc) {
                             if (fc.getResult().existsAsFile())
                                 chosen(fc.getResult());
                         });
}

void DetailView::valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int)
{
    if (parent.hasType(ids::tracks) || parent.hasType(ids::returns) || parent.hasType(ids::liveSet))
        triggerAsyncUpdate();
}

void DetailView::valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree&)
{
    if (parent.hasType(ids::tracks) || parent.hasType(ids::returns) || parent.hasType(ids::liveSet))
        triggerAsyncUpdate();
}

void DetailView::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    if (property == ids::name && tree == ownerNode())
        repaint();
}

void DetailView::paint(juce::Graphics& g)
{
    g.fillAll(colours::background);
    auto title = getLocalBounds().withTrimmedLeft(browserWidth + 4).removeFromTop(titleHeight);
    g.setColour(colours::text);
    g.setFont(juce::FontOptions(13.0f, juce::Font::bold));
    g.drawText(ownerName(), title.reduced(6, 0), juce::Justification::centredLeft);
    g.setColour(colours::dimText);
    g.setFont(juce::FontOptions(11.0f));
    g.drawText("Drag a device's title to move it. Right-click a knob to map it or type a value.", title.reduced(6, 0),
               juce::Justification::centredRight);
}

void DetailView::resized()
{
    auto area = getLocalBounds();
    browser.setBounds(area.removeFromLeft(browserWidth));
    area.removeFromLeft(4);
    area.removeFromTop(titleHeight);
    rowView.setBounds(area);
    if (row != nullptr)
    {
        const int height = std::max(120, area.getHeight() - rowView.getScrollBarThickness());
        row->setRowHeight(height);
        row->setSize(std::max(row->preferredWidth(), area.getWidth()), height);
    }
}

} // namespace lattice::debugui
