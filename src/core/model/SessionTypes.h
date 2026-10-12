#pragma once

#include <cstdint>

namespace lattice
{

/** How a clip reacts to its launch button (docs/03-m2-engine-session.md, "Launch modes").
    Repeat mode is deferred. */
enum class LaunchMode : std::uint8_t
{
    Trigger, // starts on press; release is ignored
    Gate,    // plays while held; the release is quantized like the launch
    Toggle   // a second press stops the clip
};

/** Launch quantization. Bars follow the current time signature; note values are in quarter-note
    beats. A clip set to Global uses the set's global value. */
enum class Quantization : std::int8_t
{
    Global = -1,
    None = 0,
    Bars8,
    Bars4,
    Bars2,
    Bar1,
    Half,
    Quarter,
    Eighth,
    Sixteenth
};

constexpr int numQuantizationValues = 9; // None..Sixteenth

/** Grid length in beats (quarter notes) for a quantization value, given the bar length in beats.
    0 for None (and for Global, which must be resolved first). */
constexpr double quantizationBeats(Quantization q, double beatsPerBar) noexcept
{
    switch (q)
    {
    case Quantization::Bars8:
        return 8.0 * beatsPerBar;
    case Quantization::Bars4:
        return 4.0 * beatsPerBar;
    case Quantization::Bars2:
        return 2.0 * beatsPerBar;
    case Quantization::Bar1:
        return beatsPerBar;
    case Quantization::Half:
        return 2.0;
    case Quantization::Quarter:
        return 1.0;
    case Quantization::Eighth:
        return 0.5;
    case Quantization::Sixteenth:
        return 0.25;
    case Quantization::Global:
    case Quantization::None:
        break;
    }
    return 0.0;
}

/** Simplified Live 11 follow actions (docs/03-m2-engine-session.md, "Follow actions"). "Previous",
    "Next", "First", "Last", "Any" and "Other" choose among the clips in the same track column. */
enum class FollowAction : std::uint8_t
{
    None,
    Stop,
    PlayAgain,
    Previous,
    Next,
    First,
    Last,
    Any,
    Other
};

constexpr int numFollowActions = 9;

/** What the UI (and later the Push LEDs) shows for one clip slot. */
enum class SlotState : std::uint8_t
{
    Empty,
    Stopped,
    QueuedLaunch,
    Playing,
    QueuedStop,
    QueuedRecord,
    Recording
};

} // namespace lattice
