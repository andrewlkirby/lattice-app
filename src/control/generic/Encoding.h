#pragma once

#include <juce_core/juce_core.h>

#include <optional>

namespace lattice::control::generic
{

/** How an encoder or fader sends its value (docs/06-m5-controllers.md, "Generic MIDI
    controllers"). The relative ones send a step count each message; the rest a position. */
enum class Encoding : std::uint8_t
{
    absolute,       // 0..127: the position
    twosComplement, // 1..63 clockwise, 127..65 anticlockwise (127 = -1)
    binaryOffset,   // 65..127 clockwise, 63..0 anticlockwise (64 = none)
    signMagnitude,  // 1..63 clockwise, 65..127 anticlockwise (bit 6 the sign)
    cc14,           // a 14-bit position: the MSB's CC then the LSB's
    nrpn14          // a 14-bit position as an NRPN (CC 99, 98, then 6 and 38)
};

const char* encodingName(Encoding e) noexcept;
std::optional<Encoding> encodingFromName(juce::StringRef name);
inline bool isRelative(Encoding e) noexcept
{
    return e == Encoding::twosComplement || e == Encoding::binaryOffset || e == Encoding::signMagnitude;
}

/** A relative encoder's steps from one 7-bit value (clockwise positive). */
int relativeSteps(Encoding e, int value) noexcept;

/** Works out how an encoder sends from the 7-bit values it sent turned clockwise, then
    anticlockwise (the learn wizard): an absolute one's values walk up then down; a relative one's
    cluster near the values its encoding gives a step each way. Empty if they don't say. */
std::optional<Encoding> detectEncoding(const std::vector<int>& clockwise, const std::vector<int>& anticlockwise);

} // namespace lattice::control::generic
