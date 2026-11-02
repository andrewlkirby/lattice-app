#pragma once

#include "ui/device/DeviceFrame.h"
#include "ui/widgets/Icons.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <memory>
#include <vector>

namespace lattice::ui
{

/** A chain's devices side by side (a track's, a return's, the master's, or a rack chain's), with
    room at the end for more (docs/05-m4-ui.md, 4d).
    - Devices, presets and samples dropped on it go in by the chain rules where they're dropped
      (a line shows where); a device dragged by its title bar moves there.
    - The "+" at the end, or a right-click on the empty part, offers every device and preset.
    - Clicking it makes it the chain new devices go into (M3 hands-on finding 4). */
class ChainView final : public juce::Component,
                        public juce::DragAndDropTarget,
                        public juce::FileDragAndDropTarget,
                        private juce::ValueTree::Listener,
                        private juce::AsyncUpdater
{
public:
    ChainView(DeviceContext& context, juce::ValueTree devicesNode);
    ~ChainView() override;

    const juce::ValueTree& chain() const noexcept { return devices; }
    const std::vector<std::unique_ptr<DeviceFrame>>& frames() const noexcept { return shown; }
    /** The width it needs at a height: its devices', and the room at the end. */
    int preferredWidth(int height) const;
    /** Applies pending rebuilds now, here and in nested chains (tests). */
    void flushUpdates();

    /** The index a drop at x goes to (between devices). */
    int indexAt(int x) const;
    /** The "+" at the end. */
    IconButton& addButton() noexcept { return add; }
    /** Where the drop line is (-1: none), for drawing and tests. */
    int dropLine() const noexcept { return dropIndex; }

    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;
    void resized() override;
    void mouseDown(const juce::MouseEvent&) override;

    bool isInterestedInDragSource(const SourceDetails&) override;
    void itemDragMove(const SourceDetails&) override;
    void itemDragExit(const SourceDetails&) override;
    void itemDropped(const SourceDetails&) override;
    bool isInterestedInFileDrag(const juce::StringArray& files) override;
    void fileDragMove(const juce::StringArray& files, int x, int y) override;
    void fileDragExit(const juce::StringArray& files) override;
    void filesDropped(const juce::StringArray& files, int x, int y) override;

private:
    void rebuild();
    int endWidth() const;
    void setDropIndex(int index);
    void handleAsyncUpdate() override { rebuild(); }
    void valueTreeChildAdded(juce::ValueTree& parent, juce::ValueTree&) override;
    void valueTreeChildRemoved(juce::ValueTree& parent, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged(juce::ValueTree& parent, int, int) override;

    DeviceContext& context;
    juce::ValueTree devices;
    std::vector<std::unique_ptr<DeviceFrame>> shown;
    IconButton add{"Insert Device", Icon::plus, "Inserts a device or a preset at the end of this chain."};
    int dropIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(ChainView)
};

} // namespace lattice::ui
