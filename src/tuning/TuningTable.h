#pragma once

#include <juce_core/juce_core.h>

#include <array>
#include <memory>

namespace lattice
{

/** One tuning: an absolute frequency for each of the 128 MIDI notes (docs/02-m1-tuning-core.md).
    Octave-repeating scales are a special case; the Colundi table is not one.
    Never changed after construction; shared as TuningPtr. */
struct TuningTable
{
    static constexpr int numNotes = 128;

    juce::String id;   // "builtin.12tet", "builtin.colundi", "user.<content hash>"
    juce::String name; // shown in menus
    juce::String sourceDescription;

    std::array<double, numNotes> hz{};          // 0 means "unmapped" (a .kbm can leave notes out)
    std::array<juce::String, numNotes> labels;  // "C4", or "#47 · 440.00 Hz"

    double periodRatio = 2.0;   // the scale's repeat interval; FoldDown divides by it
    int keyboardPageSize = 12;  // the computer keyboard's Z/X step (17 for Colundi)
    bool usesNoteNames = false; // labels are note names (12-TET), not table indices

    /** Frequency at a fractional note, interpolated in log space between neighbouring entries:
        hz[n] * (hz[n+1] / hz[n])^x. Clamped to the table's ends. Returns 0 next to an unmapped
        entry when the nearer neighbour is unmapped. Real-time safe. */
    double frequencyFor(double fractionalNote) const noexcept;

    /** The mapped note whose frequency is closest (in log terms) to hz, or -1 if none. */
    int nearestNoteTo(double targetHz) const noexcept;

    /** The computer keyboard's default first note: the start of the page (multiple of
        keyboardPageSize) holding the note nearest to middle C (261.63 Hz). */
    int defaultKeyboardBase() const noexcept;
};

using TuningPtr = std::shared_ptr<const TuningTable>;

/** Result of loading or parsing a tuning: a table, or an error with its 1-based line (0 if the
    error is not about one line). */
struct TuningLoadResult
{
    TuningPtr table;
    juce::String error;
    int line = 0;

    bool ok() const noexcept { return table != nullptr; }

    static TuningLoadResult success(TuningPtr t) { return {std::move(t), {}, 0}; }
    static TuningLoadResult failure(juce::String message, int lineNumber = 0)
    {
        return {nullptr, std::move(message), lineNumber};
    }
};

} // namespace lattice
