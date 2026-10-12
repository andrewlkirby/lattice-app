#pragma once

#include "tuning/TuningTable.h"

namespace lattice
{

/** Parses the Colundi table format (resources/tunings/colundi_scale.txt):
    tab-separated `# | Freq | Cents | Ratio | Name`, lines starting with '#' are headers or
    comments, data rows usually start with a tab, and exactly 128 rows with indices 0..127.

    Only the index and Freq columns are used ("440.000Hz"). Cents (scientific notation from
    row 101) and Ratio (rounded) are not reliable enough for pitch, and Name uses comma decimals;
    none of them is parsed, so they can't cause errors. Parsing never depends on the locale. */
struct ColundiTextParser
{
    struct Options
    {
        juce::String id;
        juce::String name = "Colundi";
        juce::String sourceDescription;
        double periodRatio = 2.0;  // the table itself doesn't state it
        int keyboardPageSize = 17;
    };

    static TuningLoadResult parse(const juce::String& text, const Options& options);

    /** Label used for non-12-TET tables: "#47 · 440.00 Hz". */
    static juce::String labelFor(int index, double hz);
};

} // namespace lattice
