#pragma once

#include "control/surface/Surface.h"
#include "core/RealtimeCheck.h"
#include "core/RtShared.h"
#include "engine/EngineCommands.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace lattice
{
class AudioEngine;
}

namespace lattice::control
{

/** How the pads play (docs/06-m5-controllers.md, "Global controls"): the velocity curve, pressure
    and the touch strip. One set for every surface, from the Preferences' Controllers page. */
struct Expression
{
    enum class Pressure : std::uint8_t
    {
        off,
        polyphonic, // each pad's pressure goes to its own note (on a device with only channel
                    // pressure, it's the channel's)
        channel     // the most any held pad is pressed, for the whole channel
    };
    enum class Strip : std::uint8_t
    {
        pitchBend, // springs back to the middle when let go
        modWheel,  // stays where it's left
        scroll     // moves the Note mode's pads, a row at a time
    };

    float velocityCurve = 0.0f; // -1 (soft: light hits play louder) .. 0 (linear) .. 1 (hard)
    int velocityMin = 1;        // what the lightest and hardest hits play (1..127)
    int velocityMax = 127;
    Pressure pressure = Pressure::polyphonic;
    float pressureThreshold = 0.0f; // pressure below it is none; the rest is scaled to 0..1
    Strip strip = Strip::pitchBend;
    /** On a device with MPE pads (Push 3, Virtual Push): each note on a channel of its own, with
        its own slide (pitch), timbre and pressure (docs/06-m5-controllers.md, 5g). */
    bool perNote = true;

    /** The note velocity for a pad's raw velocity (0..1]: 1..127. */
    int velocityFor(float raw) const noexcept;
    /** The pressure sent for a pad's raw pressure 0..1: 0..127. */
    int pressureFor(float raw) const noexcept;

    bool operator==(const Expression&) const = default;
};

/** What the pads do on the fast path, for the current mode (docs/06-m5-controllers.md, "Fast path
    for pads"). The controller makes one from the mode whenever what a pad does changes, and
    publishes it to the PadPlayer through RtShared; the driver's input thread reads it without
    locks. Immutable once published. */
struct PadNoteMap
{
    struct Pad
    {
        enum class Kind : std::uint8_t
        {
            none,   // the message thread handles it (the mode's own logic)
            note,   // plays `note`
            command // sends onDown (and onUp on release, if it has one): a launch or a stop
        };
        Kind kind = Kind::none;
        std::uint8_t note = 0;
        bool hasUp = false;
        /** A note pad a slide across moves the pitch from (a melodic layout's, not a drum's): the
            slide follows the notes of the pads it crosses in its row. */
        bool slides = false;
        EngineCommand onDown{};
        EngineCommand onUp{};

        bool operator==(const Pad& o) const noexcept;
    };

    /** A button that sends a command on the fast path (a scene launch). */
    struct ButtonCommand
    {
        ButtonId id;
        EngineCommand command;

        bool operator==(const ButtonCommand& o) const noexcept;
    };

    int rows = 0, columns = 0;
    std::vector<Pad> pads;                // row by row, from the top
    std::vector<ButtonCommand> buttons;   // pressing one sends its command (no release)
    std::uint8_t channel = 0;             // 0..15 (with perNote: MPE's master channel)
    std::uint8_t source = 0;              // the engine's source number for this surface
    /** The track the notes are for (TimedMidi::target: a surface keeping its own selection), or 0
        for the tracks that listen to the surface (armed). */
    std::uint32_t target = 0;
    /** Notes on channels of their own, the others than `channel` (MPE's lower zone), with their
        slide, timbre and pressure (Expression::perNote, on a device with MPE pads). */
    bool perNote = false;
    std::array<std::uint8_t, 128> velocity{}; // raw velocity (0..127) -> note velocity (1..127)
    std::array<std::uint8_t, 128> pressure{}; // raw pressure (0..127) -> what's sent (0..127)
    Expression::Pressure pressureMode = Expression::Pressure::off;
    Expression::Strip strip = Expression::Strip::pitchBend;
    /** Buttons that change what pads do while held (Shift, Delete...): while one is held, every
        pad goes to the message thread. A bit per Role. */
    std::uint64_t modifiers = 0;

    /** All pads go to the message thread. */
    static PadNoteMap empty(const SurfaceCapabilities& caps);
    /** Fills in the curves from the expression settings (an identity velocity for a device without
        velocity, so a fixed velocity plays as given). */
    void setExpression(const Expression& e, const SurfaceCapabilities& caps);
    Pad& pad(int row, int column) { return pads[static_cast<std::size_t>(row * columns + column)]; }
    void addModifier(Role role) noexcept { modifiers |= std::uint64_t{1} << static_cast<int>(role); }
    bool isModifier(Role role) const noexcept { return (modifiers >> static_cast<int>(role)) & 1u; }

    bool operator==(const PadNoteMap&) const = default;
};

/** The fast path (docs/06-m5-controllers.md, 5b): a pad press plays, or launches, from the
    driver's input thread, without waiting for the message thread. A stalled UI doesn't delay it.

    - Notes go into the engine's live MIDI queue, stamped with when they arrived, so they play at
      the matching sample of the next block. A pad remembers the note it started, so its release
      ends that note even if the layout changed meanwhile. Two pads playing the same note: the
      second press plays it again, and it ends when the last of them is let go.
    - Pressure follows the map (each note's own, or the channel's); the strip bends or modulates.
    - Per note (MPE, PadNoteMap::perNote): each note gets the channel used least recently, and
      its slide (a pitch bend over perNoteBendSteps, following the notes of the pads crossed),
      timbre (CC 74) and pressure (channel pressure) go on that channel, flagged per note. The
      strip stays on the master channel, for every note.
    - Commands (a clip or scene launch, a track stop) go into the engine's command queue.
    - While a button the map lists as a modifier is held, pads go to the message thread instead.

    play() is real-time safe: no locks, no allocation (it runs inside RealtimeCheck in tests).
    Called by one input thread at a time (the driver's); publish() on the message thread. */
class PadPlayer
{
public:
    PadPlayer(AudioEngine& engine, const SurfaceCapabilities& caps);
    ~PadPlayer();

    /** Message thread. */
    void publish(std::shared_ptr<const PadNoteMap> map);
    const PadNoteMap* published() const noexcept { return maps.published().get(); }

    /** The input thread. Plays what the map says for a pad, a button or the strip (and follows
        the modifiers), and marks the input played if it did. seconds: when it happened (MidiInputQueue::nowSeconds()'s
        clock; default now). Returns true if anything went to the engine. */
    bool play(SurfaceInput& input, double seconds = -1.0) noexcept LATTICE_NONBLOCKING;
    /** play(), then wakes what polls the engine (that part may lock, so it comes after). */
    bool handle(SurfaceInput& input, double seconds = -1.0);
    /** The input thread. Ends every note and gate the pads hold (the driver is going away). */
    void releaseAll() noexcept LATTICE_NONBLOCKING;

    /** Notes held by pads right now (for tests). */
    int heldNotes() const noexcept;

private:
    struct Held
    {
        PadNoteMap::Pad::Kind kind = PadNoteMap::Pad::Kind::none;
        std::uint8_t note = 0, channel = 0, source = 0;
        std::uint8_t master = 0; // the map's channel (with perNote, not the note's own)
        std::uint8_t lastPressure = 0;
        bool hasUp = false;
        bool channelWide = false; // its pressure is part of the channel's
        bool perNote = false;     // on a channel of its own
        std::uint32_t target = 0;
        int lastBend = 8192, lastTimbre = 64; // per note: the last sent
        EngineCommand onUp{};
    };
    bool padDown(const PadNoteMap& map, int index, float velocity, double seconds) noexcept LATTICE_NONBLOCKING;
    bool padUp(int index, double seconds) noexcept LATTICE_NONBLOCKING;
    bool padPressure(const PadNoteMap& map, int index, float pressure, double seconds) noexcept LATTICE_NONBLOCKING;
    void sendChannelPressure(const Held& h, double seconds) noexcept LATTICE_NONBLOCKING;
    bool padSlide(const PadNoteMap& map, int index, float pads, double seconds) noexcept LATTICE_NONBLOCKING;
    bool padTimbre(int index, float timbre, double seconds) noexcept LATTICE_NONBLOCKING;
    bool strip(const PadNoteMap& map, const SurfaceInput& input, double seconds) noexcept LATTICE_NONBLOCKING;
    /** A note's own channel: one holding no note, used least recently (or, all busy, the note
        started longest ago's). */
    std::uint8_t channelForNote(const PadNoteMap& map) noexcept LATTICE_NONBLOCKING;
    bool send(std::uint8_t status, std::uint8_t data1, std::uint8_t data2, int size, std::uint8_t source,
              std::uint32_t target, double seconds, std::uint8_t flags = 0) noexcept LATTICE_NONBLOCKING;
    bool send(const Held& h, std::uint8_t status, std::uint8_t data1, std::uint8_t data2, int size,
              double seconds) noexcept LATTICE_NONBLOCKING;

    AudioEngine& engine;
    RtShared<PadNoteMap> maps;
    // The input thread's own state.
    std::vector<Held> held; // by pad
    std::array<std::uint8_t, 128> noteHolders{}; // pads holding each note
    std::uint64_t heldModifiers = 0;
    std::uint8_t channelPressure = 0;
    std::array<std::uint8_t, 16> notesOnChannel{}; // per note: the held notes on each channel
    std::array<std::uint64_t, 16> channelUsed{};   // when each last started or ended a note
    std::uint64_t uses = 0;
    std::atomic<int> notesHeld{0};
    int numPads = 0;
};

} // namespace lattice::control
