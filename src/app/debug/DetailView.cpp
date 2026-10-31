#include "app/debug/DetailView.h"

#include "platform/Platform.h"

namespace lattice::debugui
{

// --- Detail view ------------------------------------------------------------------------------------------

DetailView::DetailView(SessionController& s, AudioEngine& engine, juce::PropertiesFile& settings)
    : session(s), specs(s.deviceRegistry()),
      library(presets::PresetLibrary::defaultUserRoot(platform::userDataFolder()), s.deviceRegistry()),
      context{s, engine, specs, library,
              [this](const juce::String& title, const juce::String& patterns, std::function<void(const juce::File&)> chosen) {
                  chooseFile(title, patterns, std::move(chosen));
              },
              [] {}}
{
    juce::ignoreUnused(settings);
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
    auto title = getLocalBounds().removeFromTop(titleHeight);
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
