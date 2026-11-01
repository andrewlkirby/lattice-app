#pragma once

#include "tuning/TuningTable.h"

namespace lattice
{

/** Parses the Colundi table format (resources/tunings/colundi_scale.txt):
    tab-separated `# | Freq | Cents | Ratio | Name`, lines starting with '#' are headers or
    comments, data rows usually start with a tab, and exactly 128 rows with indices 0..127.

    Only the index and Freq columns are used ("440.000Hz"). Cents (scientific notation from
    row 101) and Ratio (rounded) are not reliable enough for pitch, and Name uses comma decimals;
    none of them is parsed, so they can't cause errors. Parsing never depends on the locale.

    What the tuning editor saves (docs/05-m4-ui.md, 4f) adds, all optional, so the original file
    reads as before:
    - a sixth column, Label: the entry's label (empty: the usual "#47 · 440.00 Hz");
    - "-" as a row's Freq: the entry is unmapped (a subset leaves keys out); frequencies must
      increase from one mapped row to the next;
    - comment lines "# Period: 938.6666667" (the repeat interval, for the FoldDown high-note
      policy), "# Keyboard page: 17" (the computer keyboard's step) and "# Note names: yes"
      (labels are note names, as 12-TET's), which override the options. */
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

    /** Label used for non-12-TET tables: "#47 · 440.00 Hz"; "#47 (unmapped)" for hz 0. */
    static juce::String labelFor(int index, double hz);

    /** A table in this format, as parse() reads it back: the frequencies to nine decimals, labels
        that aren't the usual ones, and the period, page size and note names as comment lines. */
    static juce::String write(const TuningTable& table);
};

} // namespace lattice
