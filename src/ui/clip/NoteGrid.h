#pragma once

#include <juce_core/juce_core.h>

#include <optional>

namespace lattice::ui
{

/** A clip's time in Live's bars.beats.sixteenths, for the Clip view's number boxes. A beat is the
    time signature's denominator (an eighth in 6/8); times in the model are in quarter notes. */
struct BarsBeats
{
    int numerator = 4;
    int denominator = 4;

    double quartersPerBeat() const noexcept { return 4.0 / denominator; }
    double quartersPerBar() const noexcept { return numerator * quartersPerBeat(); }

    /** A position, counted from 1 ("1.1.1" is the clip's start). */
    juce::String position(double quarters) const;
    /** A length, counted from 0 ("1.0.0" is one bar). */
    juce::String length(double quarters) const;
    /** Reads "2", "2.3" or "2.3.4" (missing fields: 1 for a position, 0 for a length). nullopt if
        it isn't that. */
    std::optional<double> parsePosition(const juce::String& text) const { return parse(text, 1); }
    std::optional<double> parseLength(const juce::String& text) const { return parse(text, 0); }

private:
    juce::String format(double quarters, int origin) const;
    std::optional<double> parse(const juce::String& text, int origin) const;
};

/** The piano roll's grid (docs/05-m4-ui.md, 4c), as in Live: adaptive (following the zoom) or
    fixed, with triplets, and a snap that can be turned off. Ctrl+1 and Ctrl+2 narrow and widen it,
    Ctrl+3 switches triplets, Ctrl+4 snapping, Ctrl+5 between adaptive and fixed. Plain arithmetic.

    The values, finest first: 1/64, 1/32, 1/16, 1/8, 1/4 and 1/2 notes, then 1, 2, 4 and 8 bars. */
struct NoteGrid
{
    static constexpr int numValues = 10;
    static constexpr int minDensity = -3, maxDensity = 3;

    bool adaptive = true;
    int density = 0;    // adaptive: + narrows (finer lines), - widens
    int fixedIndex = 2; // fixed: a value (2: 1/16)
    bool triplet = false;
    bool snap = true;

    /** A value in quarter notes (without triplets). */
    static double valueStep(int index, double quartersPerBar) noexcept;
    static juce::String valueName(int index);

    /** The grid step in quarter notes at a zoom (pixels per quarter note). Adaptive: the finest
        value whose lines are at least minPixels apart (halved per density step). Triplets make it
        two thirds as long. */
    double step(double pixelsPerQuarter, double quartersPerBar, double minPixels) const noexcept;
    /** The value step() picks (an index, before triplets). */
    int valueIndex(double pixelsPerQuarter, double quartersPerBar, double minPixels) const noexcept;
    /** What the grid is now, for the piano roll's corner: "1/16", "1/8T", "1 Bar", with "(Off)"
        when snapping is off. */
    juce::String describe(double pixelsPerQuarter, double quartersPerBar, double minPixels) const;

    void narrow() noexcept;
    void widen() noexcept;

    /** A time snapped to the nearest line (unchanged with no step). */
    static double nearest(double quarters, double step) noexcept;
    /** The line at or before a time. */
    static double floor(double quarters, double step) noexcept;
};

} // namespace lattice::ui
