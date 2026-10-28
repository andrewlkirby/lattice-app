#pragma once

#include "ui/device/DeviceBody.h"
#include "ui/widgets/ControlRegistry.h"
#include "ui/widgets/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lattice::ui
{

/** One device in a chain in the Device view (docs/05-m4-ui.md, 4d): a title bar and the device's
    body under it.

    - Title bar: the on/off switch, the fold switch, the name, then hot-swap, save as preset, show
      all parameters, and a menu (rename, presets, copy, duplicate, delete). Drag the title bar to
      move the device; double-click it to fold or unfold; right-click for the menu.
    - Folded, it's a narrow strip with the switches and the name running down it.
    - Clicking it selects it (the Edit commands then act on it while the Device view has the keys).
    - A Sampler takes samples dropped on it.
    It tells the control registry where its controls live ("Release" · "Synth · 2 MIDI") and how
    to bring them into view. */
class DeviceFrame final : public juce::Component,
                          public ControlRegistry::Locator,
                          public juce::DragAndDropTarget,
                          public juce::FileDragAndDropTarget,
                          private juce::ValueTree::Listener,
                          private juce::AsyncUpdater
{
public:
    DeviceFrame(DeviceContext& context, juce::ValueTree device);
    ~DeviceFrame() override;

    const juce::ValueTree& device() const noexcept { return node; }
    DeviceBody* body() const noexcept { return content.get(); }
    bool isFolded() const;
    bool showsAllParameters() const;

    /** The width it needs at a height (its body's plus its edges; a strip when folded). */
    int preferredWidth(int height) const;
    int titleHeight() const;
    /** Unfolds it, and shows every parameter if its own panel doesn't show this one; returns the
        parameter's control (nullptr if the device has no such parameter). */
    juce::Component* revealParameter(const juce::String& parameterId);
    /** Applies pending rebuilds now, here and in nested chains (tests). */
    void flushUpdates();

    void showMenu();
    juce::PopupMenu buildMenu();
    /** Map mode started or ended: inside the rack being mapped, its parameters take clicks to map
        them instead of changing. */
    void mapModeChanged();
    /** Whether a click on a parameter maps it now. */
    bool isMapping() const;

    IconButton& powerButton() noexcept { return power; }
    IconButton& foldButton() noexcept { return fold; }
    IconButton& hotSwapButton() noexcept { return hotSwap; }
    IconButton& saveButton() noexcept { return save; }
    IconButton& allParametersButton() noexcept { return allParams; }
    /** The title bar (local coordinates; the name strip when folded). */
    juce::Rectangle<int> titleBar() const;

    // juce::Component
    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;
    void mouseDrag(const juce::MouseEvent&) override;
    void mouseDoubleClick(const juce::MouseEvent&) override;

    // ControlRegistry::Locator
    void describe(ControlRegistry::Entry& entry) const override;

    // A Sampler takes samples.
    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDropped(const SourceDetails&) override;
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

private:
    bool isSampler() const;
    void loadSample(const juce::File& file);
    void rebuildBody();
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void handleAsyncUpdate() override { rebuildBody(); }

    DeviceContext& context;
    juce::ValueTree node;
    std::unique_ptr<DeviceBody> content;
    bool builtAll = false;

    IconButton power{"On", Icon::power, "Turns the device on or off."};
    IconButton fold{"Fold", Icon::foldOpen, "Folds the device to a narrow strip (or unfolds it)."};
    IconButton hotSwap{"Hot-Swap", Icon::hotSwap, "Swaps the device for another, or for a preset: the next one you choose replaces it."};
    IconButton save{"Save Preset", Icon::save, "Saves the device, with everything in it, as a preset."};
    IconButton allParams{"Show All Parameters", Icon::allParams, "Shows every parameter instead of the device's own panel."};
    IconButton menu{"Device Menu", Icon::menu, "Rename, presets, copy, duplicate and delete."};

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DeviceFrame)
};

} // namespace lattice::ui
