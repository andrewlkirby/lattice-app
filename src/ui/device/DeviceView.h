#pragma once

#include "ui/commands/EditTarget.h"
#include "ui/device/ChainView.h"
#include "ui/session/SessionSelectionModel.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <memory>

namespace lattice::ui
{

/** The Device view (docs/05-m4-ui.md, 4d): the device chain of the selected track (or of a return,
    or the master) in a row that scrolls sideways, each device with its own panel; racks show their
    macros, chains and the selected chain's devices inline.

    - Selecting: clicking a device selects it; Left and Right move the selection along the chain.
      While the view has the keys, Duplicate, Delete, Copy, Cut, Paste and Rename act on the
      selected device.
    - Inserting where you're looking (M3 hands-on finding 4): new devices go into the chain you
      last worked in (the selected device's chain, or a rack chain you selected), after the
      selected device. An instrument where there already is one asks before it replaces it, or
      replaces it at once in hot-swap mode; it never replaces one silently.
    - Hot-swap: a device's hot-swap switch makes it the target; the next device or preset chosen
      replaces it, and the mode ends.
    - Racks: the selected chain of each rack, and map mode (a macro waiting for a parameter). */
class DeviceView final : public juce::Component,
                         public EditTarget,
                         private SessionSelectionModel::Listener,
                         private juce::ValueTree::Listener,
                         private juce::AsyncUpdater
{
public:
    DeviceView(DeviceContext& context, SessionSelectionModel& selection);
    ~DeviceView() override;

    DeviceContext& deviceContext() noexcept { return context; }

    // --- What it shows ------------------------------------------------------------------------------
    const DeviceOwner& owner() const noexcept { return shown; }
    /** The TRACK, RETURN or MASTER node shown (invalid if it's gone). */
    juce::ValueTree ownerNode() const;
    juce::String ownerName() const;
    /** The top-level chain's view (null while the owner has no chain). */
    ChainView* chainView() const noexcept { return top.get(); }
    /** The frame showing a device, anywhere in the view (null if it isn't showing). */
    DeviceFrame* frameFor(const juce::ValueTree& device) const;
    juce::Viewport& viewport() noexcept { return scroller; }

    // --- Selection ----------------------------------------------------------------------------------
    void select(const juce::ValueTree& device);
    const juce::ValueTree& selectedDevice() const noexcept { return selected; }
    /** The chain new devices go into, set by clicking in it (a rack chain, or the top level). */
    void focusChain(const juce::ValueTree& devicesNode);

    // --- Inserting ----------------------------------------------------------------------------------
    /** Where a new device goes now: the selected device's chain (after it), or the focused chain
        (at its end), or the owner's chain. */
    juce::ValueTree insertionChain() const;
    int insertionIndex() const;
    /** Inserts what a drag description names (devices, presets, samples, moves; see drag::) into a
        chain at an index (-1: the end), by the chain rules. In hot-swap mode it replaces the
        target instead. Returns false if nothing went in (refused, or waiting for an answer). */
    bool insert(const juce::String& description, const juce::ValueTree& chain, int index);
    /** insert() at the insertion point. */
    bool insertHere(const juce::String& description) { return insert(description, insertionChain(), insertionIndex()); }
    /** The menu of device types and presets that inserts at a point (the chain's "+", a right
        click on its empty part, and the Device menu's Insert). */
    juce::PopupMenu insertMenu(const juce::ValueTree& chain, int index);

    // --- Hot-swap ------------------------------------------------------------------------------------
    /** The device the next insert replaces (an invalid tree: hot-swap is off). */
    void setHotSwapTarget(const juce::ValueTree& device);
    const juce::ValueTree& hotSwapTarget() const noexcept { return swapTarget; }

    // --- Racks ---------------------------------------------------------------------------------------
    /** The rack chain shown inline (a pad, for a Drum Rack); its first chain unless one was chosen. */
    juce::ValueTree selectedChainOf(const juce::ValueTree& rack) const;
    void selectChain(const juce::ValueTree& rack, const juce::ValueTree& chain);
    /** Map mode: the next parameter clicked inside `rack` is mapped to `macro`. An invalid rack ends it. */
    void setMapMode(const juce::ValueTree& rack, int macro);
    const juce::ValueTree& mapModeRack() const noexcept { return mapRack; }
    int mapModeMacro() const noexcept { return mapMacro; }
    /** In map mode, maps the macro to a parameter of a device in the rack (one undo step). */
    bool mapTo(const juce::ValueTree& device, const juce::String& parameterId);

    // --- Finding controls ------------------------------------------------------------------------------
    /** Brings a device's parameter on screen: shows its owner, unfolds the device, opens the rack
        chains it's in, shows every parameter if its panel doesn't show this one, scrolls to it and
        flashes it. Returns the control, or nullptr if there's no such device or parameter. */
    juce::Component* revealParameter(const juce::ValueTree& device, const juce::String& parameterId);
    /** The control flashed last, and until when (for drawing the flash). */
    juce::Component* flashedControl() const noexcept { return flashed.getComponent(); }

    // --- EditTarget ----------------------------------------------------------------------------------
    bool canEdit(const juce::String& commandId) const override;
    void edit(const juce::String& commandId) override;
    /** This view, when it (or something in it) has the keys. */
    EditTarget* editTargetIfFocused();

    /** A part's width changed: everything is laid out again (on the next message-loop turn). */
    void requestLayout();
    /** Applies pending rebuilds and layouts now (tests). */
    void flushUpdates();
    /** Called when what it shows changes (the detail view shows its name). */
    std::function<void(const juce::String&)> onTitleChanged;
    /** Called when revealing a control needs the Device view on screen. */
    std::function<void()> onShowRequested;

    void paint(juce::Graphics&) override;
    void paintOverChildren(juce::Graphics&) override;
    void resized() override;
    bool keyPressed(const juce::KeyPress&) override;
    void mouseDown(const juce::MouseEvent&) override;

private:
    void selectionChanged(const SessionSelection&) override {}
    void deviceOwnerChanged(const DeviceOwner& now) override;
    void rebuild();
    void layout();
    void handleAsyncUpdate() override;
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override;
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override;
    void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override;
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override;
    void valueTreeRedirected(juce::ValueTree&) override;
    bool replaceDevice(const juce::ValueTree& target, const juce::String& description);
    bool insertNow(const juce::String& description, const juce::ValueTree& chain, int index);
    void scrollIntoView(juce::Component& c);
    void moveSelection(int delta);
    void copySelected();
    void pasteHere();
    void renameSelected();
    void titleMaybeChanged();

    DeviceContext& context;
    SessionSelectionModel& selection;
    DeviceOwner shown;
    juce::ValueTree shownChain;
    juce::Viewport scroller;
    std::unique_ptr<ChainView> top;
    juce::ValueTree selected;
    juce::ValueTree focused;
    juce::ValueTree swapTarget;
    juce::ValueTree mapRack;
    int mapMacro = -1;
    std::map<std::int64_t, std::int64_t> chainChoice; // rack ID -> chain ID shown
    juce::ValueTree clipboard;
    juce::Component::SafePointer<juce::Component> flashed;
    double flashUntil = 0.0;
    bool needsRebuild = false, needsLayout = false;
    juce::String lastTitle;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(DeviceView)
};

} // namespace lattice::ui
