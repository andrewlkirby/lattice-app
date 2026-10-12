#pragma once

#include "devices/framework/StudioDevice.h"

#include <array>
#include <cstdint>

namespace lattice
{

/** MIDI effect "lattice.arpeggiator" (docs/04-m3-devices.md, "MIDI effects").

    Plays the held notes one step at a time: up, down, up/down, in the order played, at random, or
    all together (chord). Steps fall on the transport's grid at the synced rate (computed from beats,
    so the timing is exact to the sample at any rate and block size); while the transport is stopped
    the grid runs on at the current tempo. Each step lasts `gate` of the step. The pattern repeats
    over `range` ranges, each `distance` table steps higher, and everything is transposed by
    `transpose` table steps, so it stays within the active tuning. The pattern restarts on a new
    note, on a beat interval, or never. Hold keeps playing after the keys are released. */
class ArpeggiatorDevice final : public StudioDevice
{
public:
    static constexpr const char* typeId = "lattice.arpeggiator";
    static constexpr int maxHeld = 64;
    static constexpr int maxPattern = maxHeld * 8;

    enum class Style : std::uint8_t
    {
        Up,
        Down,
        UpDown,
        Played,
        Random,
        Chord
    };

    enum class Retrigger : std::uint8_t
    {
        Off,
        Note,
        Beat
    };

    enum Param : int
    {
        style, rate, gate, range, distance, transpose, retrigger, retriggerRate, hold, numParams
    };

    ArpeggiatorDevice();

    void releaseAllNotes() noexcept override { reset(); }
    void reset() noexcept override;

protected:
    void prepare(double, int) override { reset(); }
    void process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept override;

private:
    struct HeldNote
    {
        std::uint8_t note = 0;
        std::uint8_t velocity = 0;
        std::uint8_t channel = 0;
        bool keyDown = false;
    };
    struct Playing
    {
        std::uint8_t note = 0;
        std::uint8_t channel = 0;
        double offBeat = 0.0;
    };

    void noteOn(const TimedMidi& e) noexcept LATTICE_NONBLOCKING;
    void noteOff(const TimedMidi& e) noexcept LATTICE_NONBLOCKING;
    void step(double beat, int offset, MidiBlock& out) noexcept LATTICE_NONBLOCKING;
    int buildPattern() noexcept LATTICE_NONBLOCKING;
    void endPlaying(int offset, MidiBlock& out) noexcept LATTICE_NONBLOCKING;

    std::array<HeldNote, maxHeld> held{}; // in the order played
    int numHeld = 0;
    bool anyKeyDown = false;
    std::array<std::int16_t, maxPattern> pattern{};
    std::array<std::uint8_t, maxPattern> patternVelocity{};
    int position = 0;
    bool restartPending = true;
    double lastRetriggerBeat = -1.0;
    double nextStepBeat = -1.0; // the next step on the grid; < 0 = find it from the transport
    double lastStepBeats = 0.0;
    std::array<Playing, maxHeld> playing{};
    int numPlaying = 0;
    std::uint32_t random = 0x2545f491u;
};

} // namespace lattice
