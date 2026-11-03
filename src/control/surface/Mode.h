#pragma once

#include "control/surface/PadPlayer.h"
#include "control/surface/Surface.h"
#include "engine/SessionController.h"
#include "ui/commands/CommandRegistry.h"
#include "ui/feed/EngineStateFeed.h"
#include "ui/session/SessionSelectionModel.h"

namespace lattice::control
{

/** What the modes act on and read: the same commands, session, selection and live state as the
    window (docs/06-m5-controllers.md, "SurfaceController and ModeStack"). */
struct SurfaceContext
{
    SessionController& session;
    ui::CommandRegistry& commands;
    ui::SessionSelectionModel& selection;
    ui::EngineStateFeed& feed;
};

/** The colours the LEDs use for states (0xRRGGBB): the theme's, so a pad and the Session view
    agree. Clip, track and scene colours come from the set. */
struct SurfaceColours
{
    std::uint32_t play = 0x3cc85a;
    std::uint32_t record = 0xe5383b;
    std::uint32_t white = 0xd8d8d8;
    std::uint32_t mute = 0xe0b030;  // a track that's on (lit when the track plays)
    std::uint32_t solo = 0x3d8de0;
    std::uint32_t arm = 0xe5383b;
    float dim = 0.3f;               // how bright an available but inactive button is
    float padDim = 0.22f;           // how bright a note pad outside the key or highlight is
};

enum class ModeKind : std::uint8_t
{
    session,
    note,
    device,
    mix,
    browse,
    clip
};

/** What a mode can ask of the controller it runs in. */
class ModeHost
{
public:
    virtual ~ModeHost() = default;
    virtual const SurfaceCapabilities& capabilities() const = 0;
    virtual SurfaceContext& context() = 0;
    virtual const SurfaceColours& colours() const = 0;
    virtual bool isHeld(ButtonId id) const = 0;
    bool isHeld(Role role) const { return isHeld(ButtonId{role, 0}); }
    bool shiftHeld() const { return isHeld(Role::shift); }
    /** Whether this surface moves the shared selection (and follows it), or keeps its own. */
    virtual bool followsSelection() const = 0;
    /** Something the LEDs show changed outside the feed and the set (a hold, the box). */
    virtual void requestRefresh() = 0;
    /** The track a track button (or a lower display button) selects, from the Session box: -1
        for none. */
    virtual int trackOfColumn(int column) const = 0;
    /** The settings the pads play with (the strip's mode, pressure). */
    virtual const Expression& expression() const = 0;
};

/** One mode (Session, Note, Device...). Modes are plain classes: tests drive them headlessly
    through FakeSurface. Message thread. */
class Mode
{
public:
    explicit Mode(ModeHost& h) : host(h) {}
    virtual ~Mode() = default;

    virtual ModeKind kind() const = 0;
    /** Input the controller didn't take itself (the global buttons). True if this mode used it. */
    virtual bool handle(const SurfaceInput& input) = 0;
    /** Fills in the pads and the buttons this mode uses (the controller has already drawn the
        global buttons; a mode may draw over them). */
    virtual void render(LedFrame& frame) = 0;

    virtual void enter() {}
    virtual void leave() {}
    /** The shared selection changed (only called while following it). */
    virtual void selectionChanged() {}
    /** The feed's aspects this mode's LEDs show (the controller refreshes when any changes). */
    virtual std::uint32_t aspects() const { return ui::EngineStateFeed::transportAspect | ui::EngineStateFeed::slotsAspect; }
    /** What the pads do on the fast path (the controller has made it empty and sized; a mode
        fills in its pads and the buttons that change them). Default: everything goes to handle(). */
    virtual void padMap(PadNoteMap&) {}
    /** The pads' labels and the status line. */
    virtual void describe(SurfaceText&) {}

protected:
    ModeHost& host;
    SurfaceContext& ctx() { return host.context(); }
    const SurfaceCapabilities& caps() const { return host.capabilities(); }
    /** A button's index in the frame, or -1 if the device has none. */
    int buttonIndex(ButtonId id) const { return caps().indexOf(id); }
    void setButton(LedFrame& frame, ButtonId id, LedState state) const
    {
        if (const int i = buttonIndex(id); i >= 0)
            frame.buttons[static_cast<std::size_t>(i)] = state;
    }
};

} // namespace lattice::control
