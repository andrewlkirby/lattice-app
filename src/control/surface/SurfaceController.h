#pragma once

#include "control/surface/Mode.h"

#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <memory>
#include <vector>

namespace lattice::control
{

class SessionMode;
class NoteMode;

/** Runs one connected device (docs/06-m5-controllers.md, "SurfaceController and ModeStack"): its
    current mode, the global buttons (transport, undo, Shift, the mode buttons), and its LEDs.

    The LEDs are worked out again only when something they show changes: the feed's state (on
    the tick that saw it), the set, the selection, or a held button. The driver then gets the
    lights that differ from what it shows. Nothing runs while nothing changes (idle CPU, charter
    §1), and a change shows within the frame it happens in (charter: LED response <= 1 frame).

    Message thread. The driver must outlive the controller. */
class SurfaceController final : public ModeHost,
                                private ui::EngineStateFeed::Listener,
                                private ui::SessionSelectionModel::Listener,
                                private juce::ValueTree::Listener,
                                private juce::AsyncUpdater
{
public:
    SurfaceController(SurfaceContext context, SurfaceDriver& driver);
    ~SurfaceController() override;

    /** What the device did (the driver's onInput comes here). */
    void handle(const SurfaceInput& input);

    ModeKind modeKind() const noexcept;
    /** Modes come phase by phase (5a: Session, 5b: Note); false for one that doesn't exist yet. */
    bool isAvailable(ModeKind kind) const noexcept;
    void setMode(ModeKind kind);
    SessionMode& sessionMode() noexcept { return *session; }
    NoteMode& noteMode() noexcept { return *note; }

    /** The fast path the driver's input goes through first (docs/06-m5-controllers.md, 5b). */
    PadPlayer& padPlayer() noexcept { return *player; }
    /** The pads' map as published now. */
    const PadNoteMap& padMap() const noexcept { return *published; }
    /** The source number this surface's notes carry (tracks listening to all inputs hear them). */
    std::uint8_t midiSource() const noexcept { return source; }
    void setExpression(const Expression& e);

    void setFollowsSelection(bool follow);
    void setColours(const SurfaceColours& c);

    /** Works the LEDs out now and sends what changed (the controller does this by itself; tests
        call it to see a change at once). */
    void refresh();
    const LedFrame& shownLeds() const noexcept { return shown; }
    /** How many frames have gone to the driver (a frame that changed nothing isn't sent). */
    std::uint64_t framesSent() const noexcept { return sent; }

    // ModeHost
    const SurfaceCapabilities& capabilities() const override { return driver.capabilities(); }
    SurfaceContext& context() override { return ctx; }
    const SurfaceColours& colours() const override { return palette; }
    bool isHeld(ButtonId id) const override;
    using ModeHost::isHeld;
    bool followsSelection() const override { return follow; }
    void requestRefresh() override { triggerAsyncUpdate(); }
    int trackOfColumn(int column) const override;
    const Expression& expression() const override { return expressionSettings; }

private:
    /** The transport, undo, Shift and mode buttons: true if it was one. */
    bool handleGlobal(const SurfaceInput& input);
    void renderGlobals(LedFrame& frame) const;
    Mode& current() noexcept { return *modes[static_cast<std::size_t>(active)]; }
    void listenToFeed();
    void publishPadMap();

    void engineStateChanged(std::uint32_t aspects) override;
    void selectionChanged(const ui::SessionSelection&) override;
    void deviceOwnerChanged(const ui::DeviceOwner&) override { triggerAsyncUpdate(); }
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override { triggerAsyncUpdate(); }
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { triggerAsyncUpdate(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { triggerAsyncUpdate(); }
    void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override { triggerAsyncUpdate(); }
    void handleAsyncUpdate() override { refresh(); }

    SurfaceContext ctx;
    SurfaceDriver& driver;
    SurfaceColours palette;
    std::vector<std::unique_ptr<Mode>> modes; // by ModeKind; null for those not built yet
    SessionMode* session = nullptr;
    NoteMode* note = nullptr;
    Expression expressionSettings;
    std::uint8_t source = 0;
    std::unique_ptr<PadPlayer> player;
    std::shared_ptr<const PadNoteMap> published;
    SurfaceText shownText;
    bool shownAnyText = false;
    std::size_t active = 0;
    std::vector<ButtonId> held;
    bool follow = true;
    LedFrame shown;
    bool shownAny = false;
    std::uint64_t sent = 0;

    JUCE_DECLARE_NON_COPYABLE(SurfaceController)
};

} // namespace lattice::control
