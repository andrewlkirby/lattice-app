#pragma once

#include "devices/framework/StudioDevice.h"

#include <array>
#include <cstdint>

namespace lattice
{

/** MIDI effect "lattice.chord" (docs/04-m3-devices.md, "MIDI effects"): adds up to six notes to each
    note played, at offsets in table steps (note numbers), each with its own velocity. In Colundi
    that builds chords from table intervals. A shift of 0 is off.

    Each played note remembers the notes it added, so its note-off ends exactly those even if the
    shifts changed while it was held. */
class ChordDevice final : public StudioDevice
{
public:
    static constexpr const char* typeId = "lattice.chord";
    static constexpr int numShifts = 6;

    ChordDevice();

    void releaseAllNotes() noexcept override { reset(); }
    void reset() noexcept override;

protected:
    void prepare(double, int) override {}
    void process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept override;

private:
    struct Held
    {
        std::array<std::int16_t, numShifts + 1> notes{}; // what this note sent (-1 = none)
        bool down = false;
    };
    std::array<std::array<Held, 128>, 16> held{}; // by channel and note
};

} // namespace lattice
