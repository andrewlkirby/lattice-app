#include "tuning/parsers/TunParser.h"

#include "tuning/parsers/ColundiTextParser.h"
#include "tuning/parsers/NumberParsing.h"

#include <cmath>
#include <map>

namespace lattice
{

namespace
{
constexpr double note0Hz = 8.1757989156437073336;

/** A value, without its quotes. */
juce::String unquoted(const juce::String& value)
{
    return value.trim().unquoted().trim();
}
} // namespace

TuningLoadResult TunParser::parse(const juce::String& text, juce::String id, juce::String name, juce::String sourceDescription)
{
    juce::StringArray lines;
    lines.addLines(text);

    // Section -> key -> (value, line).
    std::map<juce::String, std::map<juce::String, std::pair<juce::String, int>>> sections;
    juce::String section;
    for (int i = 0; i < lines.size(); ++i)
    {
        const auto line = lines[i].upToFirstOccurrenceOf(";", false, false).trim();
        if (line.isEmpty())
            continue;
        if (line.startsWithChar('['))
        {
            section = line.fromFirstOccurrenceOf("[", false, false).upToFirstOccurrenceOf("]", false, false).trim().toLowerCase();
            continue;
        }
        if (!line.containsChar('='))
            continue;
        const auto key = line.upToFirstOccurrenceOf("=", false, false).trim().toLowerCase().replace("\t", " ");
        sections[section][key] = {line.fromFirstOccurrenceOf("=", false, false).trim(), i + 1};
    }

    const bool exact = sections.contains("exact tuning");
    if (!exact && !sections.contains("tuning"))
        return TuningLoadResult::failure("The file has no [Tuning] or [Exact Tuning] section.");
    const auto& notes = sections[exact ? "exact tuning" : "tuning"];

    double base = note0Hz;
    if (exact)
        if (const auto it = notes.find("basefreq"); it != notes.end())
        {
            const auto value = parsing::parseDecimal(it->second.first);
            if (!value || *value <= 0.0)
                return TuningLoadResult::failure("BaseFreq '" + it->second.first + "' is not a frequency.", it->second.second);
            base = *value;
        }

    auto table = std::make_shared<TuningTable>();
    table->id = std::move(id);
    table->sourceDescription = std::move(sourceDescription);
    table->name = name.isNotEmpty() ? name : juce::String("AnaMark tuning");
    if (const auto& info = sections["info"]; info.contains("name") && unquoted(info.at("name").first).isNotEmpty())
        table->name = unquoted(info.at("name").first);
    table->usesNoteNames = false;
    for (int n = 0; n < TuningTable::numNotes; ++n)
    {
        double hz = 440.0 * std::pow(2.0, (n - 69) / 12.0);
        if (const auto it = notes.find("note " + juce::String(n)); it != notes.end())
        {
            const auto cents = parsing::parseDecimal(it->second.first);
            if (!cents)
                return TuningLoadResult::failure("'" + it->second.first + "' is not a number of cents.", it->second.second);
            hz = base * std::pow(2.0, *cents / 1200.0);
        }
        table->hz[static_cast<std::size_t>(n)] = hz;
        table->labels[static_cast<std::size_t>(n)] = ColundiTextParser::labelFor(n, hz);
    }
    return TuningLoadResult::success(std::move(table));
}

} // namespace lattice
