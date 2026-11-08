#pragma once

#include "tuning/TuningTable.h"

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace lattice::tuning_export
{

/** Tuning files and MIDI Tuning Standard messages for synths outside the app
    (docs/07-m6-plugins-external.md, 6c, "File and sysex export"). Every export carries the 128
    frequencies a table holds; none approximates a scale it can't express.

    Unmapped notes (hz 0) have no place in these formats. .tun and .scl write such a note at the
    log-interpolated pitch between its mapped neighbours (the .kbm then leaves its key unmapped);
    MTS writes it as "no change". */

// --- AnaMark .tun (version 2) ---------------------------------------------------------------------
/** A .tun file: the [Exact Tuning] section with every note's frequency as cents above BaseFreq to
    ten decimals (what readers of version 2 use), and the [Tuning] section in whole cents for
    version 1 readers. */
juce::String toTun(const TuningTable& table);

// --- Scala .scl + .kbm ----------------------------------------------------------------------------
struct ScalaFiles
{
    juce::String scl, kbm;
};
/** The table as a scale of one step per key from its first mapped note to its last, whose period
    is the ratio between them (127 steps, period hz[127]/hz[0], for Colundi), and a mapping of one key
    per step with the first note at its own frequency. Read back with ScalaParser, it gives the same
    128 frequencies. Nothing if fewer than two notes are mapped, or the last isn't above the first. */
std::optional<ScalaFiles> toScala(const TuningTable& table);

// --- MIDI Tuning Standard ---------------------------------------------------------------------------
/** A frequency in MTS's three bytes: the semitone below it (note 69 = 440 Hz) and the 14-bit
    fraction above that, rounded to the nearest (about 0.006 cent). Below note 0 it's note 0;
    at or above the top (note 127 plus 16383/16384, about 13290 Hz) or for 0 Hz it's 7F 7F 7F, which
    MTS defines as "no change". */
std::array<std::uint8_t, 3> mtsBytes(double hz);
/** The frequency three MTS bytes stand for (0 for "no change"). */
double hzFromMtsBytes(const std::array<std::uint8_t, 3>& bytes);

inline constexpr std::uint8_t allDevices = 0x7f;

/** A bulk tuning dump (non-real-time, 08 01): all 128 notes, the name in 16 ASCII characters and
    the checksum: F0 7E dev 08 01 program name... data... checksum F7 (408 bytes). */
std::vector<std::uint8_t> bulkDump(const TuningTable& table, std::uint8_t program = 0, std::uint8_t device = allDevices);
/** A single-note tuning change (real-time, 08 02) for these notes, so a synth retunes while notes
    are held: F0 7F dev 08 02 program count [note xx yy zz]... F7. At most 127 notes in a message;
    more are split into several. */
std::vector<std::vector<std::uint8_t>> noteChanges(const TuningTable& table, const std::vector<int>& notes,
                                                   std::uint8_t program = 0, std::uint8_t device = allDevices);
/** The bytes as a .syx file holds them. */
juce::MemoryBlock toSyx(const std::vector<std::uint8_t>& message);

} // namespace lattice::tuning_export
