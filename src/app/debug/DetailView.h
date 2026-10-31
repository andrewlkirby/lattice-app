#pragma once

#include "app/debug/DevicePanel.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lattice::debugui
{

/** Which chain the detail view shows. */
struct ChainOwner
{
    enum class Kind : std::uint8_t
    {
        Track,
        Return,
        Master
    };
    Kind kind = Kind::Track;
    int index = 0;

    bool operator==(const ChainOwner&) const = default;
};

/** The bottom of the session view, as Live's Detail View: the selected track's (or return's, or
    the master's) device chain. (Its device list went in M4 4e: the browser has every device,
    preset and sample.) */
class DetailView final : public juce::Component, private juce::ValueTree::Listener, private juce::AsyncUpdater
{
public:
    DetailView(SessionController& session, AudioEngine& engine, juce::PropertiesFile& settings);
    ~DetailView() override;

    void showChainOf(ChainOwner owner);
    const ChainOwner& owner() const noexcept { return shown; }
    /** The shown chain's row (null while there is none). */
    ChainRow* chainRow() const noexcept { return row.get(); }
    presets::PresetLibrary& presetLibrary() noexcept { return library; }
    /** Applies pending rebuilds now (tests; the UI gets them on the next message-loop turn). */
    void flushUpdates();

    void paint(juce::Graphics& g) override;
    void resized() override;

    static constexpr int titleHeight = 22;

private:
    juce::ValueTree ownerNode() const;
    juce::String ownerName() const;
    void rebuildRow();
    void chooseFile(const juce::String& title, const juce::String& patterns, std::function<void(const juce::File&)> chosen);

    void valueTreeRedirected(juce::ValueTree&) override { rebuildRow(); }
    void handleAsyncUpdate() override;
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree& child, int) override;
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree&) override;
    void valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property) override;
    void valueTreeChildOrderChanged(juce::ValueTree& parent, int, int) override
    {
        if (parent.hasType(ids::tracks) || parent.hasType(ids::returns))
            triggerAsyncUpdate();
    }

    SessionController& session;
    DeviceSpecs specs;
    presets::PresetLibrary library;
    EditorContext context;
    ChainOwner shown;
    juce::ValueTree shownChain;

    juce::Viewport rowView;
    std::unique_ptr<ChainRow> row;
    std::unique_ptr<juce::FileChooser> chooser;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DetailView)
};

} // namespace lattice::debugui
