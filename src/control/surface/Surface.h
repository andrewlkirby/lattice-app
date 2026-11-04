#pragma once

#include "control/surface/Display.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace lattice::control
{

// The device-independent side of a control surface (docs/06-m5-controllers.md, "Architecture"):
// what a device has, what it sends, and what it shows. Modes only ever see these types, never a
// device's own note or CC numbers, so the same modes run on every device.

/** What a button does, independent of where it is on a device. Modes refer to roles; each driver
    maps its own buttons to them. A device without a role still has every mode usable (the
    action stays available on screen, or through another route). */
enum class Role : std::uint8_t
{
    play,
    record,
    shift,
    modeSession,
    modeNote,
    modeDevice,
    modeMix,
    modeBrowse,
    modeClip,
    navUp,
    navDown,
    navLeft,
    navRight,
    scene,        // numbered: launches the scene in that row
    trackSelect,  // numbered: selects the track in that column
    displayUpper, // numbered: the buttons above the display
    displayLower, // numbered: the buttons below it
    octaveUp,
    octaveDown,
    layout,
    scale,
    fixedLength,
    duplicate,
    deleteClip,
    undo,
    quantize,
    doubleLoop,
    newClip,
    metronome,
    tapTempo,
    stopClip,
    mute,
    solo,
    stopAll,
    count
};

/** The role's stable name ("mode.session", "scene"): definition files and settings use it. */
const char* roleName(Role role) noexcept;
std::optional<Role> roleFromName(juce::StringRef name);
/** Whether a role is numbered (scene[n], trackSelect[n], displayUpper[n], displayLower[n]). */
bool isNumbered(Role role) noexcept;

/** One button: its role, and its number for a numbered role (0 is the top scene, the leftmost
    track). */
struct ButtonId
{
    Role role = Role::play;
    int index = 0;

    bool operator==(const ButtonId&) const = default;
};

/** "scene[3]", "play". */
juce::String buttonName(ButtonId id);

enum class LedKind : std::uint8_t
{
    none,   // no light
    single, // on or off (some devices: a few levels of one colour)
    rgb     // any colour (a palette device maps to its nearest entry)
};

enum class Aftertouch : std::uint8_t
{
    none,
    channel,
    polyphonic,
    mpe
};

/** What a device has (docs/06-m5-controllers.md, "SurfaceCapabilities"). Modes size themselves
    from it: a mode never assumes 8x8, 8 encoders or a display. */
struct SurfaceCapabilities
{
    juce::String name;
    int rows = 0; // the grid (0x0: none)
    int columns = 0;
    bool velocity = true;
    Aftertouch aftertouch = Aftertouch::none;
    LedKind padLeds = LedKind::rgb;

    int encoders = 0;
    bool encoderTouch = false;
    bool encodersRelative = true;
    bool tempoEncoder = false;
    /** Relative steps across a value's whole range (fine, with Shift: ten times as many). */
    int encoderResolution = 200;

    struct Button
    {
        ButtonId id;
        LedKind led = LedKind::single;
    };
    std::vector<Button> buttons;

    // The display: a pixel display (width x height), or a text display (lines x characters).
    int displayWidth = 0, displayHeight = 0;
    int textLines = 0, textCharacters = 0;
    /** The display's cells across (one per encoder, in rows): 0 for all the encoders in one row. */
    int displayColumns = 0;

    bool touchStrip = false;

    bool has(ButtonId id) const noexcept;
    bool has(Role role) const noexcept; // any button with that role
    /** How many numbered buttons of a role there are (those at 0..n-1). */
    int countOf(Role role) const noexcept;
    /** Where a button is in `buttons`, or -1. */
    int indexOf(ButtonId id) const noexcept;
    int numPads() const noexcept { return rows * columns; }
    bool hasDisplay() const noexcept { return (displayWidth > 0 && displayHeight > 0) || textLines > 0; }
    /** Cells across the display: displayColumns, or every encoder. */
    int cellsAcross() const noexcept { return displayColumns > 0 ? displayColumns : encoders; }
};

// --- Input ------------------------------------------------------------------------------------------

/** One thing the device did. Pads are in grid coordinates: row 0 at the top, column 0 on the left
    (drivers turn the device's own numbering into these). */
struct SurfaceInput
{
    enum class Kind : std::uint8_t
    {
        padDown,     // row, column, value = velocity (0..1]
        padPressure, // row, column, value = pressure 0..1
        padUp,       // row, column
        buttonDown,  // button
        buttonUp,    // button
        encoderTurn, // encoder, delta: steps (positive clockwise)
        encoderSet,  // encoder, value 0..1: an absolute control's position
        encoderTouch,
        encoderRelease,
        strip,       // value 0..1 (the bottom: 0)
        stripRelease // let go of the strip
    };
    Kind kind = Kind::padDown;
    int row = 0, column = 0;
    float value = 0.0f;
    ButtonId button{};
    int encoder = 0;
    int delta = 0;
    /** Already played by the fast path (PadPlayer) on the driver's input thread: a mode only
        shows it (lights, selection), it doesn't play or launch it again. */
    bool played = false;

    static SurfaceInput padDown(int row, int column, float velocity) { return {Kind::padDown, row, column, velocity}; }
    static SurfaceInput padPressure(int row, int column, float pressure) { return {Kind::padPressure, row, column, pressure}; }
    static SurfaceInput padUp(int row, int column) { return {Kind::padUp, row, column}; }
    static SurfaceInput buttonDown(ButtonId id) { return {Kind::buttonDown, 0, 0, 0.0f, id}; }
    static SurfaceInput buttonUp(ButtonId id) { return {Kind::buttonUp, 0, 0, 0.0f, id}; }
    static SurfaceInput strip(float position) { return {Kind::strip, 0, 0, position}; }
    static SurfaceInput encoderTurn(int index, int steps) { return {Kind::encoderTurn, 0, 0, 0.0f, {}, index, steps}; }
    static SurfaceInput encoderSet(int index, float position) { return {Kind::encoderSet, 0, 0, position, {}, index}; }
    static SurfaceInput encoderTouch(int index) { return {Kind::encoderTouch, 0, 0, 0.0f, {}, index}; }
    static SurfaceInput encoderRelease(int index) { return {Kind::encoderRelease, 0, 0, 0.0f, {}, index}; }
    static SurfaceInput stripRelease() { return {Kind::stripRelease}; }
};

// --- LEDs -------------------------------------------------------------------------------------------

/** One light: a colour (0xRRGGBB; 0 is off) and how it moves. Blinking and pulsing follow the
    beat (Animation's periods), so every device and Virtual Push move together. */
struct LedState
{
    enum class Animation : std::uint8_t
    {
        solid,
        pulse, // brightness swells and fades once a beat
        blink  // on for the first half of each half-beat
    };
    std::uint32_t rgb = 0;
    Animation animation = Animation::solid;

    bool isOff() const noexcept { return rgb == 0; }
    bool operator==(const LedState&) const = default;

    static LedState off() { return {}; }
    static LedState solid(std::uint32_t c) { return {c, Animation::solid}; }
    static LedState pulse(std::uint32_t c) { return {c, Animation::pulse}; }
    static LedState blink(std::uint32_t c) { return {c, Animation::blink}; }
};

/** The beats one cycle of an animation takes. */
inline constexpr double pulseBeats = 1.0;
inline constexpr double blinkBeats = 0.5;
/** An animated LED's brightness (0..1) at a beat position. */
float animationLevel(LedState::Animation animation, double beat) noexcept;

/** A colour scaled towards black (factor 0..1), for dim LEDs. */
std::uint32_t dimmed(std::uint32_t rgb, float factor) noexcept;
/** From a juce ARGB colour to an LED's 0xRRGGBB. */
inline std::uint32_t rgbOf(std::uint32_t argb) noexcept { return argb & 0xffffffu; }

/** Every light on a device: the pads (row by row, from the top) and the buttons (in the order of
    SurfaceCapabilities::buttons). */
struct LedFrame
{
    std::vector<LedState> pads;
    std::vector<LedState> buttons;

    /** All off, sized for a device. */
    static LedFrame blank(const SurfaceCapabilities& caps);
    LedState& pad(int row, int column, int columns) { return pads[static_cast<std::size_t>(row * columns + column)]; }

    /** What changed since `before` (sized the same; anything else counts as all changed). */
    struct Changes
    {
        std::vector<int> pads;    // indices into pads
        std::vector<int> buttons; // indices into buttons
        bool empty() const noexcept { return pads.empty() && buttons.empty(); }
    };
    Changes changesFrom(const LedFrame& before) const;
    bool anyAnimated() const noexcept;
    bool operator==(const LedFrame&) const = default;
};

/** Words a device can show beside its lights: a label on each pad (Virtual Push draws them;
    hardware ignores them) and a status line (the pressed notes, the Scale menu's choices). */
struct SurfaceText
{
    std::vector<juce::String> pads; // row by row, like LedFrame::pads; empty: no labels
    juce::String status;

    bool operator==(const SurfaceText&) const = default;
};

// --- Drivers ----------------------------------------------------------------------------------------

class PadPlayer;

/** A device, real or on screen (docs/06-m5-controllers.md, "Drivers"). The controller sends it
    LED frames with what changed; it sends the controller its input.

    Input goes through the fast path first, on the driver's own input thread: `pads` (set by the
    controller) plays notes and launches clips straight into the engine, then the input goes on
    to the controller on the message thread (onInput), marked as played. A driver whose input
    arrives on the message thread (Virtual Push) does both with deliver(); one with its own input
    thread calls pads->handle() there and hands the input to the message thread itself. */
class SurfaceDriver
{
public:
    virtual ~SurfaceDriver() = default;

    virtual const SurfaceCapabilities& capabilities() const = 0;
    /** Shows a frame; `changed` lists what differs from the frame shown before, so a driver
        sends only that (the first frame after connecting lists everything). */
    virtual void showLeds(const LedFrame& frame, const LedFrame::Changes& changed) = 0;
    /** Shows the pads' labels and the status line (only called when they change). */
    virtual void showText(const SurfaceText&) {}
    /** Shows what the display shows (only called when it changes, and only for a device with
        a display). */
    virtual void showDisplay(const DisplayFrame&) {}

    /** Set by the controller: the device's input goes here (message thread). */
    std::function<void(const SurfaceInput&)> onInput;
    /** Set by the controller: the fast path (the driver's input thread). */
    PadPlayer* pads = nullptr;

    /** For input that arrives on the message thread: the fast path, then the controller. */
    void deliver(SurfaceInput input);
};

} // namespace lattice::control
