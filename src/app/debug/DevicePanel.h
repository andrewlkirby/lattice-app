#pragma once

#include "app/debug/DeviceUi.h"
#include "devices/presets/Presets.h"
#include "engine/AudioEngine.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace lattice::debugui
{

/** What the device editor needs from the app. */
struct EditorContext
{
    SessionController& session;
    AudioEngine& engine;
    DeviceSpecs& specs;
    presets::PresetLibrary& presets;
    /** Asks for a file with the OS chooser (one at a time). */
    std::function<void(const juce::String& title, const juce::String& patterns,
                       std::function<void(const juce::File&)> chosen)>
        chooseFile;
    /** Called after a preset was saved (the device list shows it). */
    std::function<void()> presetsChanged;

    LiveSet& set() const { return session.set(); }
    juce::UndoManager& undo() const { return session.undoManager(); }
    const DeviceRegistry& registry() const { return specs.devices(); }
};

class ChainRow;

/** One device in a chain (docs/04-m3-devices.md, "3f", "Device panels"): a header with the on/off
    switch, name, preset menu and remove button (drag it to move the device), then every parameter
    group as a labelled section of controls built from the device's ParameterSpecs. Racks add
    their macros, variations, chain list (Drum Racks: the pad grid) and the selected chain's
    devices; the Sampler adds its sample file; a device with a sidechain input adds the source. */
class DevicePanel final : public juce::Component,
                          public juce::DragAndDropTarget,
                          public juce::FileDragAndDropTarget,
                          private juce::ValueTree::Listener,
                          private juce::AsyncUpdater
{
public:
    DevicePanel(EditorContext& context, juce::ValueTree device, int height);
    ~DevicePanel() override;

    const juce::ValueTree& device() const noexcept { return node; }
    int preferredWidth() const;
    /** Applies pending rebuilds now, here and in nested chains (tests; the UI gets them on the
        next message-loop turn). */
    void flushUpdates();
    /** The selected chain's devices (racks), or null. */
    ChainRow* innerChain() const noexcept { return innerRow.get(); }
    /** The row's height changed: lay the controls out for it (and size to the new width). */
    void setPanelHeight(int height);
    /** Called when preferredWidth() changes (a nested chain grew). */
    std::function<void()> onWidthChanged;

    void paint(juce::Graphics& g) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent& e) override;
    void mouseDrag(const juce::MouseEvent& e) override;

    // Samples dropped on a Sampler load into it.
    bool isInterestedInDragSource(const SourceDetails& details) override;
    void itemDropped(const SourceDetails& details) override;
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    static constexpr int headerHeight = 24;

private:
    class Section;
    class MacroSection;
    class ChainList;
    class PadGrid;
    class SidechainSection;
    class SampleSection;

    void build();
    void refresh();
    static int sectionWidth(const juce::Component& section, int bodyHeight);
    void showPresetMenu();
    void loadSample(const juce::File& file);
    bool isSampler() const;
    void selectChain(const juce::ValueTree& chain);
    /** The racks this device sits in, innermost first (for "Map to macro"). */
    std::vector<juce::ValueTree> enclosingRacks() const;
    void addMappingItems(juce::PopupMenu& menu, const juce::String& parameterId);

    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override { needsRebuild(); }
    void needsRebuild();
    void handleAsyncUpdate() override;

    EditorContext& context;
    juce::ValueTree node;
    juce::ValueTree rootNode; // the set's root, listened to for mapping and macro changes
    int panelHeight;
    bool rack = false;
    bool drumRack = false;

    juce::TextButton enable{juce::String::fromUTF8("\xe2\x97\x8f")}; // ●
    juce::Label title;
    juce::TextButton presetButton{juce::String::fromUTF8("\xe2\x96\xbc")}; // ▼
    juce::TextButton remove{juce::String::fromUTF8("\xc3\x97")};           // ×

    std::vector<std::unique_ptr<juce::Component>> sections;
    std::vector<ParameterControl*> controls;
    juce::ValueTree selectedChain;
    std::unique_ptr<ChainRow> innerRow;
    std::unique_ptr<juce::Viewport> innerView;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DevicePanel)
};

/** A chain's devices side by side (a track's, a return's, the master's, or a rack chain's).
    Devices, presets and samples dropped on it go in by the chain rules (devices/ChainEditing.h),
    where they're dropped; devices dragged by their header move. */
class ChainRow final : public juce::Component,
                       public juce::DragAndDropTarget,
                       public juce::FileDragAndDropTarget,
                       private juce::ValueTree::Listener,
                       private juce::AsyncUpdater
{
public:
    ChainRow(EditorContext& context, juce::ValueTree devicesNode, int height);
    ~ChainRow() override;

    const juce::ValueTree& chain() const noexcept { return devices; }
    int preferredWidth() const;
    void flushUpdates();
    const std::vector<std::unique_ptr<DevicePanel>>& devicePanels() const noexcept { return panels; }
    std::function<void()> onWidthChanged;
    void setRowHeight(int height);

    /** Inserts what a drag description names (see drag::) at index (-1: the end). */
    void insertFromDescription(const juce::String& description, int index);

    void paint(juce::Graphics& g) override;
    void paintOverChildren(juce::Graphics& g) override;
    void resized() override;

    bool isInterestedInDragSource(const SourceDetails& details) override;
    void itemDragMove(const SourceDetails& details) override;
    void itemDragExit(const SourceDetails& details) override;
    void itemDropped(const SourceDetails& details) override;
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragMove(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

    static constexpr int emptyWidth = 160;

private:
    void rebuild();
    int indexAt(int x) const;
    void layout();
    void handleAsyncUpdate() override { rebuild(); }

    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree&) override
    {
        if (parent == devices)
            triggerAsyncUpdate();
    }
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int) override
    {
        if (parent == devices)
            triggerAsyncUpdate();
    }
    void valueTreeChildOrderChanged(juce::ValueTree& parent, int, int) override
    {
        if (parent == devices)
            triggerAsyncUpdate();
    }

    EditorContext& context;
    juce::ValueTree devices;
    int rowHeight;
    std::vector<std::unique_ptr<DevicePanel>> panels;
    int dropIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChainRow)
};

} // namespace lattice::debugui
