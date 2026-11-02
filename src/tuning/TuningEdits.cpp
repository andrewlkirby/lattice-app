#include "tuning/TuningEdits.h"

#include "tuning/TrackTuning.h"
#include "tuning/parsers/ColundiTextParser.h"
#include "tuning/parsers/NumberParsing.h"

#include <algorithm>
#include <cmath>

namespace lattice::tuning_edits
{
namespace
{
constexpr int numNotes = TuningTable::numNotes;

/** The table with every entry unmapped (name, period, page size and note names kept). */
TuningTable emptyLike(const TuningTable& table)
{
    TuningTable out;
    out.id = table.id;
    out.name = table.name;
    out.sourceDescription = table.sourceDescription;
    out.periodRatio = table.periodRatio;
    out.keyboardPageSize = table.keyboardPageSize;
    out.usesNoteNames = table.usesNoteNames;
    for (int i = 0; i < numNotes; ++i)
        out.labels[static_cast<std::size_t>(i)] = ColundiTextParser::labelFor(i, 0.0);
    return out;
}

/** Entry `from` of `table` moved to key `to` of `out` at `hz`, its label with it unless it's the
    usual one. */
void place(TuningTable& out, int to, const TuningTable& table, int from, double hz)
{
    const auto& label = table.labels[static_cast<std::size_t>(from)];
    const bool usual = label.isEmpty() || label == ColundiTextParser::labelFor(from, table.hz[static_cast<std::size_t>(from)]);
    out.hz[static_cast<std::size_t>(to)] = hz;
    out.labels[static_cast<std::size_t>(to)] = usual ? ColundiTextParser::labelFor(to, hz) : label;
}
} // namespace

TuningTable shifted(const TuningTable& table, int offset)
{
    auto out = emptyLike(table);
    for (int to = 0; to < numNotes; ++to)
    {
        const int from = to - offset;
        if (from >= 0 && from < numNotes && table.hz[static_cast<std::size_t>(from)] > 0.0)
            place(out, to, table, from, table.hz[static_cast<std::size_t>(from)]);
    }
    return out;
}

TuningTable multiplied(const TuningTable& table, double ratio)
{
    auto out = emptyLike(table);
    for (int i = 0; i < numNotes; ++i)
        if (const double hz = table.hz[static_cast<std::size_t>(i)]; hz > 0.0)
            place(out, i, table, i, hz * ratio);
    return out;
}

std::optional<double> parseRatio(const juce::String& typed)
{
    auto text = typed.trim().removeCharacters(" ");
    std::optional<double> value;
    if (text.endsWithIgnoreCase("cents") || text.endsWithIgnoreCase("c"))
    {
        text = text.endsWithIgnoreCase("cents") ? text.dropLastCharacters(5) : text.dropLastCharacters(1);
        if (text.startsWithChar('+'))
            text = text.substring(1);
        if (const auto cents = parsing::parseDecimal(text))
            value = std::exp2(*cents / 1200.0);
    }
    else if (text.containsChar('/'))
    {
        const auto num = parsing::parseDecimal(text.upToFirstOccurrenceOf("/", false, false));
        const auto den = parsing::parseDecimal(text.fromFirstOccurrenceOf("/", false, false));
        if (num && den && *den != 0.0)
            value = *num / *den;
    }
    else
        value = parsing::parseDecimal(text);
    if (!value || !std::isfinite(*value) || *value <= 0.0)
        return std::nullopt;
    return value;
}

TuningTable subset(const TuningTable& table, std::vector<int> indices, int startKey)
{
    std::erase_if(indices, [&](int i) { return i < 0 || i >= numNotes || table.hz[static_cast<std::size_t>(i)] <= 0.0; });
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    auto out = emptyLike(table);
    int key = std::max(0, startKey);
    for (const int from : indices)
    {
        if (key >= numNotes)
            break;
        place(out, key++, table, from, table.hz[static_cast<std::size_t>(from)]);
    }
    return out;
}

int centredStart(int count)
{
    return std::clamp(60 - count / 2, 0, std::max(0, numNotes - count));
}

std::vector<int> anchorIndices(const TuningTable& table)
{
    std::vector<int> out;
    for (int i = 0; i < numNotes; ++i)
        if (table.isAnchor(i))
            out.push_back(i);
    return out;
}

Check check(const TuningTable& table, double sampleRate)
{
    Check result;
    const auto add = [&](int row, bool error, juce::String message) {
        result.problems.push_back({row, error, std::move(message)});
        if (error)
            ++result.errors;
    };
    const double limit = sampleRate * TrackTuning::maxPlayableFraction;
    int lastMapped = -1;
    for (int i = 0; i < numNotes; ++i)
    {
        const double hz = table.hz[static_cast<std::size_t>(i)];
        if (hz == 0.0)
            continue; // unmapped
        if (!std::isfinite(hz) || hz < 0.0)
        {
            add(i, true, "The frequency must be a positive number.");
            continue;
        }
        if (lastMapped >= 0 && hz <= table.hz[static_cast<std::size_t>(lastMapped)])
            add(i, true, "Frequencies must increase: this isn't above #" + juce::String(lastMapped) + " ("
                             + juce::String(table.hz[static_cast<std::size_t>(lastMapped)], 3) + " Hz).");
        else if (limit > 0.0 && hz >= limit)
        {
            ++result.aboveLimit;
            add(i, false, "Above " + juce::String(limit / 1000.0, 1) + " kHz, too high to play at "
                              + juce::String(sampleRate / 1000.0, 1) + " kHz: muted, or folded down.");
        }
        lastMapped = i;
    }
    if (lastMapped < 0)
    {
        result.problems.insert(result.problems.begin(), {-1, true, "No entry is mapped."});
        ++result.errors;
    }
    return result;
}

} // namespace lattice::tuning_edits
