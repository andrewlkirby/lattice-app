#pragma once

#include "control/generic/Encoding.h"
#include "control/surface/Surface.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <optional>
#include <vector>

namespace lattice::control::generic
{

/** A device's MIDI message for one control: a note or a CC on a channel (0..15). */
struct MidiAddress
{
    enum class Type : std::uint8_t
    {
        note,
        cc
    };
    Type type = Type::note;
    int channel = 0;
    int number = 0;

    bool operator==(const MidiAddress&) const = default;
};

/** Bytes with placeholders, for a sysex a definition sends: "F0 00 20 29 02 0C 03 03 {id} {r7}
    {g7} {b7} F7". Placeholders: {id} (the LED's number), {r7} {g7} {b7} (0..127), {r8hi} {r8lo}
    and the same for g and b (a 0..255 colour as its top bit and low seven bits), {line} (a text
    display's line, from 0) and {text} (the line's characters, padded with spaces to its width). */
struct ByteTemplate
{
    struct Token
    {
        enum class Kind : std::uint8_t
        {
            byte,
            id,
            r7,
            g7,
            b7,
            r8hi,
            r8lo,
            g8hi,
            g8lo,
            b8hi,
            b8lo,
            line,
            text
        };
        Kind kind = Kind::byte;
        std::uint8_t value = 0;
        bool operator==(const Token&) const = default;
    };
    std::vector<Token> tokens;

    bool empty() const noexcept { return tokens.empty(); }
    struct Values
    {
        int id = 0;
        std::uint32_t rgb = 0;
        int line = 0;
        juce::String text;
        int width = 0;
    };
    /** The bytes, placeholders filled in (7-bit, as MIDI data must be). */
    std::vector<std::uint8_t> fill(const Values& values) const;
    bool operator==(const ByteTemplate&) const = default;
};

/** How a definition's lights are driven (docs/06-m5-controllers.md, "Generic MIDI controllers"). */
struct LedOutput
{
    enum class Kind : std::uint8_t
    {
        none,
        single,  // on or off: a note or CC at onValue or offValue (blinkValue: blinking, if it can)
        palette, // a note or CC whose value picks a colour from the palette
        sysex    // a colour in a sysex message (placeholders as ByteTemplate)
    };
    Kind kind = Kind::none;
    int onValue = 127, offValue = 0, blinkValue = -1;
    juce::String paletteName;
    std::vector<std::uint32_t> palette;      // index -> 0xRRGGBB (palette): unknown entries 0
    int channel = -1;                         // the channel it's sent on (-1: the control's own)
    int blinkChannel = -1, pulseChannel = -1; // palette: the channel that makes it blink or pulse
    ByteTemplate sysex;

    /** The colour's palette index (nearest by perceived distance; an off light 0). */
    int nearest(std::uint32_t rgb) const;
    bool operator==(const LedOutput&) const = default;
};

/** A device's text display: lines x characters, each line sent with a template. */
struct TextDisplay
{
    int lines = 0, characters = 0;
    ByteTemplate line;
    bool operator==(const TextDisplay&) const = default;
};

/** One `*.surface.json` file, read (docs/06-m5-controllers.md, "Generic MIDI controllers"): what the
    device has, which MIDI each part sends, and how its lights and display are driven. Parts given
    in the file only (no code), so a new device needs no build. */
struct SurfaceDefinition
{
    juce::String name, manufacturer;
    /** Tested on the device itself. Until then a definition is "community/unverified". */
    bool verified = false;
    /** A starting point for a device of one's own (a keyboard with knobs): never matched to a
        device by its ports, only chosen. */
    bool isTemplate = false;
    juce::String source; // where its numbers came from (a programmer's reference)
    juce::String notes;

    /** Ports it is on (regular expressions, matched against port names; the first input port that
        matches is used, and the output named most like it). */
    juce::StringArray inputPorts, outputPorts;
    /** The reply to a MIDI identity request starts with these bytes (after F0 7E <id> 06 02), if
        given: tells two devices with the same port names apart. */
    std::vector<std::uint8_t> identity;
    /** Sent on connecting (programmer mode, say) and on letting go. */
    std::vector<juce::MidiMessage> init, exit;

    struct Grid
    {
        int rows = 0, columns = 0;
        std::vector<MidiAddress> pads; // row by row from the top-left
        bool velocity = true;
        Aftertouch aftertouch = Aftertouch::none;
        LedOutput led;
        bool operator==(const Grid&) const = default;
    } grid;

    struct Button
    {
        ButtonId id;
        MidiAddress address;
        LedKind ledKind = LedKind::none;
        LedOutput led; // kind none: the definition's buttonLed (for its ledKind)
        bool operator==(const Button&) const = default;
    };
    std::vector<Button> buttons;
    /** The lights buttons use when they don't say: for single-colour ones and for RGB ones. */
    LedOutput singleLed, rgbLed;

    struct Encoder
    {
        MidiAddress address;
        Encoding encoding = Encoding::absolute;
        int lsb = -1;           // cc14: the low seven bits' CC (usually number + 32)
        int nrpn = -1;          // nrpn14: the parameter number
        std::optional<MidiAddress> touch; // a note while touched
        bool operator==(const Encoder&) const = default;
    };
    std::vector<Encoder> encoders;
    int encoderResolution = 0; // relative steps across a whole range; 0: the default

    TextDisplay display;

    /** What the device has, for the modes. */
    SurfaceCapabilities capabilities() const;

    /** Reads a definition; the result says what's wrong, with where ("buttons[3].role: no role
        'foo'"). */
    static juce::Result parse(const juce::String& json, SurfaceDefinition& out);
    /** The definitions that come with the app (each unverified until tested on its device). */
    static std::vector<SurfaceDefinition> included();

    bool operator==(const SurfaceDefinition&) const = default;
};

/** The palettes a definition can name ("novation", "akai"): approximations of the devices' own
    128-colour tables until checked on a device; unknown entries are 0 (never chosen). */
std::vector<std::uint32_t> namedPalette(juce::StringRef name);

} // namespace lattice::control::generic
