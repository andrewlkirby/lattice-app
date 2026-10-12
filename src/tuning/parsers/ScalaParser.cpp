#include "tuning/parsers/ScalaParser.h"

#include "tuning/parsers/ColundiTextParser.h"
#include "tuning/parsers/NumberParsing.h"

#include <algorithm>
#include <cmath>

namespace lattice
{
namespace
{
/** Non-comment lines with their 1-based line numbers. Comment lines start with '!'. */
struct Line
{
    juce::String text;
    int number;
};

std::vector<Line> contentLines(const juce::String& text)
{
    juce::StringArray raw;
    raw.addLines(text);
    std::vector<Line> out;
    for (int i = 0; i < raw.size(); ++i)
    {
        const auto line = raw[i].trimCharactersAtEnd("\r");
        if (!line.startsWithChar('!'))
            out.push_back({line, i + 1});
    }
    return out;
}

juce::String firstToken(const juce::String& line)
{
    return line.trim().upToFirstOccurrenceOf(" ", false, false).upToFirstOccurrenceOf("\t", false, false);
}

/** A pitch line: cents if it contains '.', otherwise a ratio "a/b" or a whole number "a". */
std::optional<double> parsePitch(const juce::String& token)
{
    if (token.containsChar('.'))
    {
        const auto cents = parsing::parseDecimal(token);
        if (!cents)
            return std::nullopt;
        return std::pow(2.0, *cents / 1200.0);
    }

    const auto numText = token.upToFirstOccurrenceOf("/", false, false);
    const auto denText = token.containsChar('/') ? token.fromFirstOccurrenceOf("/", false, false) : juce::String("1");
    const auto num = parsing::parseInteger(numText);
    const auto den = parsing::parseInteger(denText);
    if (!num || !den || *num <= 0 || *den <= 0)
        return std::nullopt;
    return static_cast<double>(*num) / static_cast<double>(*den);
}

std::optional<int> parseSignedInt(const juce::String& token)
{
    const bool negative = token.startsWithChar('-');
    const auto value = parsing::parseInteger(negative ? token.substring(1) : token);
    if (!value)
        return std::nullopt;
    return static_cast<int>(negative ? -*value : *value);
}

int floorDiv(int a, int b)
{
    int q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0)))
        --q;
    return q;
}

/** Frequency ratio of any scale degree (negative or beyond one period). */
double ratioOfDegree(const ScalaParser::Scale& scale, int degree)
{
    const int n = static_cast<int>(scale.ratios.size());
    const int q = floorDiv(degree, n);
    const int r = degree - q * n;
    const double period = scale.ratios.back();
    return std::pow(period, q) * (r == 0 ? 1.0 : scale.ratios[static_cast<std::size_t>(r - 1)]);
}
} // namespace

ScalaParser::ScaleResult ScalaParser::parseScale(const juce::String& text)
{
    const auto lines = contentLines(text);
    if (lines.empty())
        return {std::nullopt, "The .scl file is empty.", 0};

    // The description is the first non-comment line, even if blank. After it, blank lines carry
    // nothing (most files end with one), so they are skipped.
    Scale scale;
    scale.description = lines[0].text.trim();
    std::vector<Line> rest;
    for (std::size_t i = 1; i < lines.size(); ++i)
        if (lines[i].text.trim().isNotEmpty())
            rest.push_back(lines[i]);
    if (rest.empty())
        return {std::nullopt, "The .scl file is missing its note count.", lines.back().number};

    const auto count = parsing::parseInteger(firstToken(rest[0].text));
    if (!count || *count < 1)
        return {std::nullopt, "The note count '" + rest[0].text.trim() + "' is not a positive whole number.",
                rest[0].number};
    if (rest.size() - 1 != static_cast<std::size_t>(*count))
        return {std::nullopt,
                "The file lists " + juce::String(static_cast<int>(rest.size()) - 1) + " pitches but says it has "
                    + juce::String(static_cast<int>(*count)) + ".",
                0};

    for (std::size_t i = 1; i < rest.size(); ++i)
    {
        const auto& line = rest[i];
        const auto ratio = parsePitch(firstToken(line.text));
        if (!ratio || *ratio <= 0.0)
            return {std::nullopt, "'" + line.text.trim() + "' is not a pitch (cents with a '.', or a ratio).",
                    line.number};
        scale.ratios.push_back(*ratio);
    }

    if (scale.ratios.back() <= 1.0)
        return {std::nullopt, "The last pitch (the period) must be above 1/1.", rest.back().number};

    return {std::move(scale), {}, 0};
}

ScalaParser::MappingResult ScalaParser::parseMapping(const juce::String& text)
{
    auto lines = contentLines(text);
    // Blank lines carry no meaning in a .kbm.
    lines.erase(std::remove_if(lines.begin(), lines.end(), [](const Line& l) { return l.text.trim().isEmpty(); }),
                lines.end());
    if (lines.size() < 7)
        return {std::nullopt, "The .kbm file needs at least 7 values.", 0};

    KeyboardMapping m;
    const auto intAt = [&](std::size_t i) { return parseSignedInt(firstToken(lines[i].text)); };
    const auto fail = [&](std::size_t i, const juce::String& what) {
        return MappingResult{std::nullopt, "Invalid " + what + ": '" + lines[i].text.trim() + "'.", lines[i].number};
    };

    const auto size = intAt(0), first = intAt(1), last = intAt(2), middle = intAt(3), ref = intAt(4);
    const auto refHz = parsing::parseDecimal(firstToken(lines[5].text));
    const auto octave = intAt(6);
    if (!size || *size < 0)
        return fail(0, "map size");
    if (!first || *first < 0 || *first > 127)
        return fail(1, "first note");
    if (!last || *last < *first || *last > 127)
        return fail(2, "last note");
    if (!middle)
        return fail(3, "middle note");
    if (!ref || *ref < 0 || *ref > 127)
        return fail(4, "reference note");
    if (!refHz || *refHz <= 0.0)
        return fail(5, "reference frequency");
    if (!octave || *octave < 0)
        return fail(6, "octave degree");

    m.mapSize = *size;
    m.firstNote = *first;
    m.lastNote = *last;
    m.middleNote = *middle;
    m.referenceNote = *ref;
    m.referenceHz = *refHz;
    m.formalOctaveDegree = *octave;

    for (int i = 0; i < m.mapSize; ++i)
    {
        const auto index = static_cast<std::size_t>(7 + i);
        if (index >= lines.size())
        {
            m.map.push_back(-1); // the format allows trailing entries to be left out (unmapped)
            continue;
        }
        const auto token = firstToken(lines[index].text);
        if (token.equalsIgnoreCase("x"))
        {
            m.map.push_back(-1);
            continue;
        }
        const auto degree = parseSignedInt(token);
        if (!degree)
            return fail(index, "mapping entry");
        m.map.push_back(*degree);
    }

    return {std::move(m), {}, 0};
}

TuningLoadResult ScalaParser::makeTable(const Scale& scale, const KeyboardMapping& mapping, juce::String id,
                                        juce::String name, juce::String sourceDescription)
{
    const int scaleSize = static_cast<int>(scale.ratios.size());
    const int octaveDegree = mapping.formalOctaveDegree > 0 ? mapping.formalOctaveDegree : scaleSize;

    // Scale degree for a note, or nullopt if unmapped.
    const auto degreeOf = [&](int note) -> std::optional<int> {
        const int offset = note - mapping.middleNote;
        if (mapping.mapSize == 0)
            return offset;
        const int q = floorDiv(offset, mapping.mapSize);
        const int i = offset - q * mapping.mapSize;
        const int entry = mapping.map[static_cast<std::size_t>(i)];
        if (entry < 0)
            return std::nullopt;
        return entry + q * octaveDegree;
    };

    const auto refDegree = degreeOf(mapping.referenceNote);
    if (!refDegree)
        return TuningLoadResult::failure("The .kbm reference note is unmapped.");
    const double middleHz = mapping.referenceHz / ratioOfDegree(scale, *refDegree);

    auto table = std::make_shared<TuningTable>();
    table->id = std::move(id);
    table->name = std::move(name);
    table->sourceDescription = std::move(sourceDescription);
    table->periodRatio = ratioOfDegree(scale, octaveDegree);
    table->keyboardPageSize = mapping.mapSize > 0 ? mapping.mapSize : scaleSize;
    table->usesNoteNames = false;

    for (int note = 0; note < TuningTable::numNotes; ++note)
    {
        const auto degree = degreeOf(note);
        const bool inRange = note >= mapping.firstNote && note <= mapping.lastNote;
        const double hz = (inRange && degree) ? middleHz * ratioOfDegree(scale, *degree) : 0.0;
        table->hz[static_cast<std::size_t>(note)] = hz;
        table->labels[static_cast<std::size_t>(note)] =
            hz > 0.0 ? ColundiTextParser::labelFor(note, hz) : "#" + juce::String(note) + " (unmapped)";
    }

    return TuningLoadResult::success(std::move(table));
}

TuningLoadResult ScalaParser::parse(const juce::String& sclText, const juce::String& kbmText, juce::String id,
                                    juce::String name, juce::String sourceDescription)
{
    const auto scale = parseScale(sclText);
    if (!scale.scale)
        return TuningLoadResult::failure(scale.error, scale.line);

    KeyboardMapping mapping;
    if (kbmText.trim().isNotEmpty())
    {
        auto parsed = parseMapping(kbmText);
        if (!parsed.mapping)
            return TuningLoadResult::failure(".kbm: " + parsed.error, parsed.line);
        mapping = std::move(*parsed.mapping);
    }

    if (name.isEmpty())
        name = scale.scale->description.isNotEmpty() ? scale.scale->description : juce::String("Scala scale");
    return makeTable(*scale.scale, mapping, std::move(id), std::move(name), std::move(sourceDescription));
}

} // namespace lattice
