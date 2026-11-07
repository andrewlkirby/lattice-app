#pragma once

#include "control/surface/Mode.h"
#include "core/LatencyStats.h"

#include <juce_data_structures/juce_data_structures.h>
#include <juce_events/juce_events.h>

#include <memory>
#include <optional>
#include <vector>

namespace lattice::control
{

class SessionMode;
class NoteMode;
class DeviceMode;
class MixMode;
class ClipMode;
class BrowseMode;

/** Runs one connected device (docs/06-m5-controllers.md, "SurfaceController and ModeStack"): its
    modes, the global buttons (transport, undo, Shift, the mode buttons), its LEDs and display.

    Two modes run at once on a device with encoders: a pad mode (Session or Note) on the grid,
    and an encoder mode (Device or Mix) on the encoders, the display and the buttons along it, as
    on a Push. On a device without encoders Device and Mix take the grid instead (its columns as
    faders). The encoders edit through an EncoderBank: a turn is one undo step.

    Selection (docs/06-m5-controllers.md, 5g): by default the controller follows the window's
    selection (the shared one, as every other surface that follows it) and moves it. Not
    following, it keeps its own (a copy of the shared one when it stops following), and its
    modes act on that: the box, the track Note mode plays, the device Device mode edits. Its
    notes then go to its own track (PadNoteMap::target), armed or not.

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

    /** The mode on the grid. */
    ModeKind modeKind() const noexcept;
    /** The mode on the encoders and the display (none on a device without encoders). */
    std::optional<ModeKind> encoderModeKind() const noexcept;
    /** Whether a mode is running, on the grid or the encoders. */
    bool isShown(ModeKind kind) const noexcept;
    /** Modes come phase by phase (5a: Session, 5b: Note, 5c: Device, Mix, 5d: Clip, Browse); false
        for one that doesn't exist yet (Browse, without a browser to drive). */
    bool isAvailable(ModeKind kind) const noexcept;
    /** Device, Mix, Clip and Browse go on the encoders (or the grid, without encoders); Session and
        Note on the grid. */
    void setMode(ModeKind kind);
    SessionMode& sessionMode() noexcept { return *session; }
    NoteMode& noteMode() noexcept { return *note; }
    DeviceMode& deviceMode() noexcept { return *device; }
    MixMode& mixMode() noexcept { return *mix; }
    ClipMode& clipMode() noexcept { return *clipModePtr; }
    /** Null without a browser to drive. */
    BrowseMode* browseMode() noexcept { return browseModePtr; }
    EncoderBank& encoderBank() noexcept { return *bank; }
    /** What the display shows now (empty for a device without one). */
    const DisplayFrame& shownDisplay() const noexcept { return shownFrame; }

    /** The fast path the driver's input goes through first (docs/06-m5-controllers.md, 5b). */
    PadPlayer& padPlayer() noexcept { return *player; }
    /** The pads' map as published now. */
    const PadNoteMap& padMap() const noexcept { return *published; }
    /** The source number this surface's notes carry (tracks listening to all inputs hear them). */
    std::uint8_t midiSource() const noexcept { return source; }
    void setExpression(const Expression& e);

    void setFollowsSelection(bool follow);
    /** The selection the modes act on: the shared one, or the controller's own. */
    ui::SessionSelectionModel& selection() noexcept { return context().selection; }
    void setColours(const SurfaceColours& c);

    /** Works the LEDs out now and sends what changed (the controller does this by itself; tests
        call it to see a change at once). */
    void refresh();
    const LedFrame& shownLeds() const noexcept { return shown; }
    /** How many frames have gone to the driver (a frame that changed nothing isn't sent). */
    std::uint64_t framesSent() const noexcept { return sent; }
    /** From something asking for a refresh (an input, a hold, the set or the selection changing)
        to its LEDs, display and text handed to the driver, in ms (docs/06-m5-controllers.md, 5g:
        the charter's ≤ 1 frame). A feed tick refreshes at once, within its frame. */
    const LatencyStats& responseTimes() const noexcept { return response; }
    LatencyStats& responseTimes() noexcept { return response; }
    /** How long refresh() takes, in ms (part of the response, and of the message thread's frame). */
    LatencyStats& refreshCost() noexcept { return costs; }

    // ModeHost
    const SurfaceCapabilities& capabilities() const override { return driver.capabilities(); }
    SurfaceContext& context() override { return follow ? ctx : own; }
    const SurfaceColours& colours() const override { return palette; }
    bool isHeld(ButtonId id) const override;
    using ModeHost::isHeld;
    bool followsSelection() const override { return follow; }
    void requestRefresh() override { later(); }
    int trackOfColumn(int column) const override;
    const Expression& expression() const override { return expressionSettings; }
    EncoderBank& encoders() override { return *bank; }
    DeviceSpecs& specs() override { return *deviceSpecs; }
    std::vector<int> tracksInView(int count) const override;
    void scrollTracks(int columns) override;
    void pressTrack(int track) override;
    LedState trackLed(int track) const override;

private:
    /** The transport, undo, Shift and mode buttons: true if it was one. */
    bool handleGlobal(const SurfaceInput& input);
    void renderGlobals(LedFrame& frame) const;
    Mode& current() noexcept { return *modes[static_cast<std::size_t>(active)]; }
    /** The encoder mode, or null. */
    Mode* knobs() noexcept { return knob < modes.size() ? modes[knob].get() : nullptr; }
    /** The mode the encoders and the display belong to: the encoder mode, or the grid's (with no
        encoder mode, or while it takes them: held steps). */
    Mode& encoderOwner() noexcept { return knobs() != nullptr && !current().ownsEncoders() ? *knobs() : current(); }
    void handleEncoder(const SurfaceInput& input);
    /** The tempo encoder (docs/06-m5-controllers.md, "Global controls"). */
    void turnTempo(int steps);
    void listenToFeed();
    void publishPadMap();

    void engineStateChanged(std::uint32_t aspects) override;
    void selectionChanged(const ui::SessionSelection& now) override;
    /** The modes see the selection anew (it changed, or which one they use did). */
    void showSelection();
    /** New (Shift: a scene), on the selection the modes use. */
    void createNew(bool scene);
    void deviceOwnerChanged(const ui::DeviceOwner&) override { later(); }
    void deviceSelected(std::int64_t) override { later(); }
    void valueTreePropertyChanged(juce::ValueTree&, const juce::Identifier&) override { later(); }
    void valueTreeChildAdded(juce::ValueTree&, juce::ValueTree&) override { later(); }
    void valueTreeChildRemoved(juce::ValueTree&, juce::ValueTree&, int) override { later(); }
    void valueTreeChildOrderChanged(juce::ValueTree&, int, int) override { later(); }
    void handleAsyncUpdate() override { refresh(); }
    /** A refresh soon (the next message), timed from the first ask. */
    void later()
    {
        if (pendingSince < 0.0)
            pendingSince = juce::Time::getMillisecondCounterHiRes();
        triggerAsyncUpdate();
    }

    SurfaceContext ctx; // the shared selection's
    ui::SessionSelectionModel ownSelection;
    SurfaceContext own; // the same with the controller's own selection
    SurfaceDriver& driver;
    SurfaceColours palette;
    std::vector<std::unique_ptr<Mode>> modes; // by ModeKind; null for those not built yet
    SessionMode* session = nullptr;
    NoteMode* note = nullptr;
    DeviceMode* device = nullptr;
    MixMode* mix = nullptr;
    ClipMode* clipModePtr = nullptr;
    BrowseMode* browseModePtr = nullptr;
    int fixedBefore = 1; // the length Fixed Length goes back to
    double lastTempoTurn = -1.0e9; // ms (getMillisecondCounterHiRes)
    std::unique_ptr<DeviceSpecs> ownSpecs;
    DeviceSpecs* deviceSpecs = nullptr;
    std::unique_ptr<EncoderBank> bank;
    DisplayFrame shownFrame;
    bool shownAnyFrame = false;
    Expression expressionSettings;
    std::uint8_t source = 0;
    std::unique_ptr<PadPlayer> player;
    std::shared_ptr<const PadNoteMap> published;
    SurfaceText shownText;
    bool shownAnyText = false;
    std::uint32_t listened = 0; // the feed's aspects listened to
    std::size_t active = 0;
    std::size_t knob = static_cast<std::size_t>(-1); // the encoder mode (none: -1)
    std::vector<ButtonId> held;
    bool follow = true;
    LedFrame shown;
    bool shownAny = false;
    std::uint64_t sent = 0;
    LatencyStats response, costs;
    double pendingSince = -1.0; // ms: the first ask since the last refresh (-1: none)

    JUCE_DECLARE_NON_COPYABLE(SurfaceController)
};

} // namespace lattice::control
