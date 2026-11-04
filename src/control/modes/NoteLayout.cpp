#include "control/modes/NoteLayout.h"

#include "tuning/TuningTable.h"
#include "ui/clip/ClipActions.h"

#include <algorithm>

namespace lattice::control
{

const std::vector<Scale>& scales()
{
    static const std::vector<Scale> all{
        {"Major", {0, 2, 4, 5, 7, 9, 11}},
        {"Minor", {0, 2, 3, 5, 7, 8, 10}},
        {"Dorian", {0, 2, 3, 5, 7, 9, 10}},
        {"Mixolydian", {0, 2, 4, 5, 7, 9, 10}},
        {"Lydian", {0, 2, 4, 6, 7, 9, 11}},
        {"Phrygian", {0, 1, 3, 5, 7, 8, 10}},
        {"Locrian", {0, 1, 3, 5, 6, 8, 10}},
        {"Harmonic Minor", {0, 2, 3, 5, 7, 8, 11}},
        {"Melodic Minor", {0, 2, 3, 5, 7, 9, 11}},
        {"Major Pentatonic", {0, 2, 4, 7, 9}},
        {"Minor Pentatonic", {0, 3, 5, 7, 10}},
        {"Blues", {0, 3, 5, 6, 7, 10}},
        {"Whole Tone", {0, 2, 4, 6, 8, 10}},
        {"Diminished", {0, 2, 3, 5, 6, 8, 9, 11}},
    };
    return all;
}

namespace
{
const Scale& scaleOf(int index)
{
    const auto& all = scales();
    return all[static_cast<std::size_t>(std::clamp(index, 0, static_cast<int>(all.size()) - 1))];
}

int floorDiv(int a, int b)
{
    return a >= 0 ? a / b : -((-a + b - 1) / b);
}

int pitchClass(int note)
{
    return ((note % 12) + 12) % 12;
}
} // namespace

juce::String pitchClassName(int pc)
{
    static const char* const names[]{"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    return names[pitchClass(pc)];
}

// --- Notes ------------------------------------------------------------------------------------------

namespace
{
int rawNoteAt(const NoteLayout& l, int bottomRow, int column)
{
    switch (l.kind)
    {
    case NoteLayout::Kind::table: return l.origin + column * l.columnStep + bottomRow * l.rowOffset;
    case NoteLayout::Kind::chromatic: return l.origin + column + bottomRow * l.rowOffset;
    case NoteLayout::Kind::inKey:
    {
        const auto& steps = scaleOf(l.scale).steps;
        const int size = static_cast<int>(steps.size());
        const int degree = column + bottomRow * l.rowOffset;
        return l.origin + 12 * floorDiv(degree, size) + steps[static_cast<std::size_t>(((degree % size) + size) % size)];
    }
    }
    return -1;
}
} // namespace

int NoteLayout::noteAt(int row, int column, int rows) const noexcept
{
    const int n = rawNoteAt(*this, rows - 1 - row, column);
    return n >= 0 && n <= 127 ? n : -1;
}

int NoteLayout::sequencerNote(int fromBottom) const noexcept
{
    auto rows = *this;
    rows.rowOffset = 1;
    rows.columnStep = 1;
    const int n = rawNoteAt(rows, fromBottom + sequencerRow, 0);
    return n >= 0 && n <= 127 ? n : -1;
}

NoteLayout NoteLayout::sequencerMoved(int byRows, int shown) const noexcept
{
    auto next = *this;
    next.sequencerRow += byRows;
    for (int k = 0; k < std::max(1, shown); ++k)
        if (next.sequencerNote(k) >= 0)
            return next;
    return *this;
}

int NoteLayout::sequencerOctave(int shown) const noexcept
{
    switch (kind)
    {
    case Kind::table: return std::max(1, shown);
    case Kind::inKey: return static_cast<int>(scaleOf(scale).steps.size());
    case Kind::chromatic: return 12;
    }
    return 12;
}

bool NoteLayout::isRoot(int note) const noexcept
{
    return pitchClass(note) == pitchClass(root);
}

bool NoteLayout::inScale(int note) const noexcept
{
    const int step = pitchClass(note - root);
    const auto& steps = scaleOf(scale).steps;
    return std::find(steps.begin(), steps.end(), step) != steps.end();
}

std::pair<int, int> NoteLayout::span(int rows, int columns) const noexcept
{
    return {rawNoteAt(*this, 0, 0), rawNoteAt(*this, std::max(0, rows - 1), std::max(0, columns - 1))};
}

int NoteLayout::octaveDistance(int rows) const noexcept
{
    if (kind != Kind::table)
        return 12;
    return octaveStep == OctaveStep::page ? rowOffset * std::max(1, rows) : rowOffset;
}

bool NoteLayout::canShift(int direction, int rows, int columns) const noexcept
{
    if (direction == 0)
        return false;
    auto next = *this;
    next.origin += direction * octaveDistance(rows);
    const auto [lo, hi] = next.span(rows, columns);
    return hi >= 0 && lo <= 127;
}

NoteLayout NoteLayout::shifted(int direction, int rows, int columns) const noexcept
{
    if (!canShift(direction, rows, columns))
        return *this;
    auto next = *this;
    next.origin += direction * octaveDistance(rows);
    return next;
}

NoteLayout NoteLayout::scrolled(int byRows, int rows, int columns) const noexcept
{
    auto next = *this;
    next.origin += byRows * (kind == Kind::inKey ? 12 : rowOffset);
    const auto [lo, hi] = next.span(rows, columns);
    if (hi < 0 || lo > 127)
        return *this;
    return next;
}

NoteLayout NoteLayout::withRoot(int newRoot) const noexcept
{
    auto next = *this;
    next.root = pitchClass(newRoot);
    if (kind != Kind::table)
        next.origin = origin - pitchClass(root) + next.root;
    return next;
}

// --- Presets ----------------------------------------------------------------------------------------

std::vector<std::pair<juce::String, int>> NoteLayout::rowPresets(Kind kind, int columns)
{
    std::vector<std::pair<juce::String, int>> presets;
    switch (kind)
    {
    case Kind::table:
        presets.emplace_back("Sequential", columns);
        for (const int overlap : {3, 5, 7})
            if (columns - overlap > 0)
                presets.emplace_back("Overlap " + juce::String(overlap), columns - overlap);
        break;
    case Kind::inKey:
        presets.emplace_back("4ths", 3);
        presets.emplace_back("3rds", 2);
        presets.emplace_back("Sequential", columns);
        break;
    case Kind::chromatic:
        presets.emplace_back("4ths", 5);
        presets.emplace_back("3rds", 4);
        presets.emplace_back("Sequential", columns);
        break;
    }
    return presets;
}

std::vector<NoteLayout::Kind> NoteLayout::kindsFor(const TuningTable& tuning)
{
    if (tuning.usesNoteNames)
        return {Kind::inKey, Kind::chromatic, Kind::table};
    return {Kind::table};
}

juce::String NoteLayout::kindName(Kind kind)
{
    switch (kind)
    {
    case Kind::table: return "Table";
    case Kind::inKey: return "In Key";
    case Kind::chromatic: return "Chromatic";
    }
    return {};
}

NoteLayout NoteLayout::defaultFor(const TuningTable& tuning)
{
    NoteLayout l;
    if (tuning.usesNoteNames)
    {
        l.kind = Kind::inKey;
        l.origin = 36;
        l.rowOffset = 3;
        return l;
    }
    l.kind = Kind::table;
    l.origin = tuning.defaultKeyboardBase();
    l.rowOffset = 5;
    return l;
}

NoteLayout NoteLayout::withKind(Kind k, const TuningTable& tuning) const
{
    const auto kinds = kindsFor(tuning);
    if (std::find(kinds.begin(), kinds.end(), k) == kinds.end() || k == kind)
        return *this;
    auto next = *this;
    next.kind = k;
    switch (k)
    {
    case Kind::table:
        next.rowOffset = 5;
        next.columnStep = 1;
        break;
    case Kind::inKey:
    case Kind::chromatic:
        // The bottom-left plays the root, in the octave it's in now.
        next.origin = 12 * floorDiv(origin - pitchClass(root), 12) + pitchClass(root);
        next.rowOffset = k == Kind::inKey ? 3 : 5;
        break;
    }
    return next;
}

// --- Saving -----------------------------------------------------------------------------------------

juce::String NoteLayout::toString() const
{
    const char* kindText = kind == Kind::table ? "table" : kind == Kind::inKey ? "inKey" : "chromatic";
    return juce::String("kind=") + kindText + ";origin=" + juce::String(origin) + ";step=" + juce::String(columnStep) +
           ";row=" + juce::String(rowOffset) + ";root=" + juce::String(root) + ";scale=" + scaleOf(scale).name +
           ";octave=" + (octaveStep == OctaveStep::page ? "page" : "row") + ";sequencer=" + (sequencer ? "1" : "0") +
           ";drumPads=" + (drumPads ? "1" : "0") + ";seqRow=" + juce::String(sequencerRow) + ";rate=" + stepRates()[static_cast<std::size_t>(std::clamp(stepRate, 0, static_cast<int>(stepRates().size()) - 1))].first;
}

const std::vector<std::pair<juce::String, double>>& NoteLayout::stepRates()
{
    return ui::clipActions::grids(); // the same as Quantize's
}

double NoteLayout::stepLength() const noexcept
{
    const auto& rates = stepRates();
    return rates[static_cast<std::size_t>(std::clamp(stepRate, 0, static_cast<int>(rates.size()) - 1))].second;
}

NoteLayout NoteLayout::fromString(const juce::String& text, const NoteLayout& fallback)
{
    auto l = fallback;
    for (const auto& part : juce::StringArray::fromTokens(text, ";", {}))
    {
        const auto key = part.upToFirstOccurrenceOf("=", false, false).trim();
        const auto value = part.fromFirstOccurrenceOf("=", false, false).trim();
        const bool number = value.containsOnly("-0123456789") && value.isNotEmpty();
        if (key == "kind")
        {
            if (value == "table") l.kind = Kind::table;
            else if (value == "inKey") l.kind = Kind::inKey;
            else if (value == "chromatic") l.kind = Kind::chromatic;
        }
        else if (key == "origin" && number) l.origin = std::clamp(value.getIntValue(), -1024, 1024);
        else if (key == "step" && number) l.columnStep = std::clamp(value.getIntValue(), 1, maxRowOffset);
        else if (key == "row" && number) l.rowOffset = std::clamp(value.getIntValue(), 1, maxRowOffset);
        else if (key == "root" && number) l.root = pitchClass(value.getIntValue());
        else if (key == "scale")
        {
            const auto& all = scales();
            for (std::size_t i = 0; i < all.size(); ++i)
                if (value == all[i].name)
                    l.scale = static_cast<int>(i);
        }
        else if (key == "octave") l.octaveStep = value == "page" ? OctaveStep::page : OctaveStep::row;
        else if (key == "sequencer") l.sequencer = value == "1";
        else if (key == "seqRow" && number) l.sequencerRow = std::clamp(value.getIntValue(), -1024, 1024);
        else if (key == "drumPads") l.drumPads = value == "1";
        else if (key == "rate")
        {
            const auto& rates = stepRates();
            for (std::size_t i = 0; i < rates.size(); ++i)
                if (value == rates[i].first)
                    l.stepRate = static_cast<int>(i);
        }
    }
    return l;
}

} // namespace lattice::control
