#pragma once

#include "control/surface/Surface.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_graphics/juce_graphics.h>

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace lattice::control::push
{

// Ableton Push 2's MIDI and display protocol, from Ableton's "Push 2 MIDI and Display Interface
// Manual" (github.com/Ableton/push-interface, revision 1.1, firmware 1.0.60) and its
// Push2-map.json. Every number here is the manual's; the section it comes from is named beside
// it. Push 3 in controller mode is taken to be the same (unverified: see docs/06-m5-controllers.md,
// "Ableton Push 3").

// --- USB (Display Interface: USB Display Interface Access, Allocating Libusb Transfers) --------
inline constexpr int vendorId = 0x2982;
inline constexpr int push2ProductId = 0x1967;
/** Push 3: from community captures, not Ableton's manual (unverified). */
inline constexpr int push3ProductId = 0x1969;
inline constexpr int displayInterface = 0;
inline constexpr std::uint8_t displayEndpoint = 0x01; // bulk out

// --- The display (Display Interface Protocol) ------------------------------------------------------
inline constexpr int displayWidth = 960, displayHeight = 160;
/** Each line: 1920 bytes of pixels and 128 filler bytes, so no line border falls inside a
    512-byte USB buffer. */
inline constexpr int lineBytes = 2048;
inline constexpr int frameBytes = lineBytes * displayHeight; // 327,680: 640 buffers of 512
inline constexpr std::array<std::uint8_t, 16> frameHeader{0xff, 0xcc, 0xaa, 0x88, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
/** The signal shaping pattern every line's bytes are XORed with, first byte first. */
inline constexpr std::array<std::uint8_t, 4> xorPattern{0xe7, 0xf3, 0xe7, 0xff};
/** The display shows black when no frame arrives for 2 s: a frame is sent again before that. */
inline constexpr double displayTimeoutSeconds = 2.0;

/** A colour (0xRRGGBB) as the display's 16 bits: blue in the top five, green in the middle six,
    red in the low five (Pixel Color Encoding). */
constexpr std::uint16_t pixel(std::uint32_t rgb) noexcept
{
    const auto r = (rgb >> 16) & 0xffu, g = (rgb >> 8) & 0xffu, b = rgb & 0xffu;
    return static_cast<std::uint16_t>(((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3));
}

/** An image (any size: drawn at 960 x 160, the parts beyond left black) as the frame's 327,680
    bytes, after the header: each line's pixels little-endian, then its filler, all XORed. */
void encodeFrame(const juce::Image& image, std::uint8_t* out);
std::vector<std::uint8_t> encodeFrame(const juce::Image& image);

// --- MIDI (MIDI Mapping; Push2-map.json) -----------------------------------------------------------
/** Pads: notes 36 (bottom left) to 99 (top right), eight to a row. Row 0 is the top. */
inline constexpr int firstPadNote = 36;
constexpr int padNote(int row, int column) noexcept { return firstPadNote + (7 - row) * 8 + column; }
struct PadPlace
{
    int row = 0, column = 0;
};
std::optional<PadPlace> padOf(int note) noexcept;

/** The eight encoders over the display: CCs 71 to 78, touched as notes 0 to 7. The master
    encoder: CC 79, note 8; the swing encoder: CC 15, note 9; the tempo encoder: CC 14, note 10
    (detented: 18 steps a turn). The touch strip: pitch bend, touched as note 12. */
inline constexpr int firstEncoderCc = 71, encoderCount = 8;
inline constexpr int masterEncoderCc = 79, swingEncoderCc = 15, tempoEncoderCc = 14;
inline constexpr int masterTouchNote = 8, swingTouchNote = 9, tempoTouchNote = 10, stripTouchNote = 12;
/** A relative encoder's 7-bit two's complement value as steps (positive: clockwise). */
constexpr int encoderSteps(int value) noexcept { return value < 64 ? value : value - 128; }

/** A touch strip pitch bend (0..16320, the middle at 8192; only the top eight bits change) as
    0..1, with the middle exactly at 0.5. */
float stripPosition(int pitchBend) noexcept;

/** A button: its CC, the role it plays, and whether its LED is RGB (otherwise white). */
struct Button
{
    int cc = 0;
    ButtonId id;
    bool rgb = false;
};
/** Every button Lattice gives a role (the others, such as Select, Page and User, pass on to MIDI
    Map). Scene 1 (the top) is CC 43; the buttons above the display are CCs 102 to 109, those
    below 20 to 27. */
const std::vector<Button>& buttons();
/** The button on a CC, or null. */
const Button* buttonOn(int cc) noexcept;
/** The CC of the button with a role, or -1. */
int ccOf(ButtonId id) noexcept;

/** The LED animation a channel asks for (LED Animation): 0 none; the beat-synced pulse and blink
    Lattice's animations match (a pulse a quarter note, a blink an eighth). */
inline constexpr int pulseChannel = 9; // pulsing, a quarter
inline constexpr int blinkChannel = 13; // blinking, an eighth
constexpr int animationChannel(LedState::Animation a) noexcept
{
    return a == LedState::Animation::pulse ? pulseChannel : a == LedState::Animation::blink ? blinkChannel : 0;
}

// --- Sysex (Sysex Commands) -----------------------------------------------------------------------
/** F0 00 21 1D 01 01 <command> <arguments> F7. */
juce::MidiMessage command(std::uint8_t id, std::initializer_list<std::uint8_t> arguments = {});

/** Palette entry (0x03): index's colour for RGB LEDs and its level for white ones (each 0..255,
    sent as seven bits and one). */
juce::MidiMessage setPaletteEntry(int index, std::uint32_t rgb, int white);
juce::MidiMessage reapplyPalette(); // 0x05
juce::MidiMessage setLedBrightness(int brightness);     // 0x06, 0..127
juce::MidiMessage setDisplayBrightness(int brightness); // 0x08, 0..255
enum class MidiMode : std::uint8_t
{
    live = 0,
    user = 1,
    dual = 2
};
juce::MidiMessage setMidiMode(MidiMode mode); // 0x0A
juce::MidiMessage setAftertouchMode(bool polyphonic); // 0x1E
/** Touch strip flags (0x17): Push lights it as a point, returns to the middle and sends pitch
    bend: the default, 0x68. */
inline constexpr std::uint8_t stripDefault = 0x68;
juce::MidiMessage setTouchStripConfiguration(std::uint8_t flags);

/** The identity request (Device Inquiry), and whether a reply is a Push's: Ableton's ID and
    family 0x1967 (Push 2: member 2). */
juce::MidiMessage identityRequest();
bool isPushIdentity(const juce::MidiMessage& reply) noexcept;

} // namespace lattice::control::push
