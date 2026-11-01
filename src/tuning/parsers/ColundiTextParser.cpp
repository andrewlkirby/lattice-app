#include "tuning/parsers/ColundiTextParser.h"

#include "tuning/parsers/NumberParsing.h"

#include <cmath>
#include <optional>

namespace lattice
{

juce::String ColundiTextParser::labelFor(int index, double hz)
{
    static const juce::String dot = juce::String::fromUTF8("\xc2\xb7"); // U+00B7 middle dot
    if (hz <= 0.0)
        return "#" + juce::String(index) + " (unmapped)";
    return "#" + juce::String(index) + " " + dot + " " + juce::String(hz, 2) + " Hz";
}

namespace
{
/** The value of a comment line "# Key: value", or nullopt if the line isn't one for that key. */
std::optional<juce::String> directive(const juce::String& line, const char* key)
{
    const auto body = line.trimStart().substring(1).trim();
    const juce::String prefix = juce::String(key) + ":";
    if (!body.startsWithIgnoreCase(prefix))
        return std::nullopt;
    return body.substring(prefix.length()).trim();
}

/** A frequency to nine decimals, without trailing zeros past the third ("440.000", "29.97003"). */
juce::String hzText(double hz)
{
    auto text = juce::String(hz, 9);
    while (text.endsWithChar('0') && text.length() - text.indexOfChar('.') > 4)
        text = text.dropLastCharacters(1);
    return text;
}
} // namespace

juce::String ColundiTextParser::write(const TuningTable& table)
{
    juce::String out;
    out << "# " << table.name << "\n";
    out << "# Period: " << juce::String(table.periodRatio, 9) << "\n";
    out << "# Keyboard page: " << table.keyboardPageSize << "\n";
    if (table.usesNoteNames)
        out << "# Note names: yes\n";
    out << "#\tFreq\tCents\tRatio\tName\tLabel\n";
    const double base = table.baseHz();
    for (int i = 0; i < TuningTable::numNotes; ++i)
    {
        const double hz = table.hz[static_cast<std::size_t>(i)];
        const auto& label = table.labels[static_cast<std::size_t>(i)];
        const bool usual = label.isEmpty() || label == labelFor(i, hz);
        out << "\t" << i << "\t";
        if (hz > 0.0)
            out << hzText(hz) << "Hz\t" << juce::String(1200.0 * std::log2(hz / base), 3) << "\t"
                << juce::String(hz / base, 3);
        else
            out << "-\t-\t-";
        out << "\t\t" << (usual ? juce::String() : label) << "\n";
    }
    return out;
}

TuningLoadResult ColundiTextParser::parse(const juce::String& text, const Options& options)
{
    if (text.trim().isEmpty())
        return TuningLoadResult::failure("The file is empty.");

    auto table = std::make_shared<TuningTable>();
    table->id = options.id;
    table->name = options.name;
    table->sourceDescription = options.sourceDescription;
    table->periodRatio = options.periodRatio;
    table->keyboardPageSize = options.keyboardPageSize;
    table->usesNoteNames = false;

    juce::StringArray lines;
    lines.addLines(text); // handles \n and \r\n
    int rows = 0;
    int lastMapped = -1;

    for (int i = 0; i < lines.size(); ++i)
    {
        const int lineNumber = i + 1;
        const auto line = lines[i].trimCharactersAtEnd("\r");
        if (line.trim().isEmpty())
            continue;
        if (line.trimStart().startsWithChar('#'))
        {
            if (const auto period = directive(line, "Period"))
            {
                const auto value = parsing::parseDecimal(*period);
                if (!value || *value <= 1.0)
                    return TuningLoadResult::failure("The period '" + *period + "' is not a number above 1.", lineNumber);
                table->periodRatio = *value;
            }
            else if (const auto page = directive(line, "Keyboard page"))
            {
                const auto value = parsing::parseInteger(*page);
                if (!value || *value < 1 || *value > TuningTable::numNotes)
                    return TuningLoadResult::failure("The keyboard page '" + *page + "' is not a whole number from 1 to 128.",
                                                     lineNumber);
                table->keyboardPageSize = static_cast<int>(*value);
            }
            else if (const auto names = directive(line, "Note names"))
                table->usesNoteNames = names->equalsIgnoreCase("yes");
            continue;
        }

        auto fields = juce::StringArray::fromTokens(line, "\t", "");
        if (!fields.isEmpty() && fields[0].trim().isEmpty())
            fields.remove(0); // data rows start with a tab
        for (auto& f : fields)
            f = f.trim();

        if (rows >= TuningTable::numNotes)
            return TuningLoadResult::failure("More than 128 rows.", lineNumber);

        const auto index = parsing::parseInteger(fields[0]);
        if (!index)
            return TuningLoadResult::failure("The row number '" + fields[0] + "' is not a whole number.", lineNumber);
        if (*index != rows)
            return TuningLoadResult::failure("Expected row " + juce::String(rows) + ", found row "
                                                 + juce::String(*index) + ".",
                                             lineNumber);

        if (fields.size() < 2 || fields[1].isEmpty())
            return TuningLoadResult::failure("Row " + juce::String(rows) + " has no frequency.", lineNumber);

        const auto label = fields.size() > 5 ? fields[5] : juce::String();
        if (fields[1] == "-")
        {
            table->labels[static_cast<std::size_t>(rows)] = label.isNotEmpty() ? label : labelFor(rows, 0.0);
            ++rows;
            continue; // unmapped
        }

        auto freqText = fields[1];
        if (!freqText.endsWithIgnoreCase("Hz"))
            return TuningLoadResult::failure("The frequency '" + freqText + "' does not end in Hz.", lineNumber);
        freqText = freqText.dropLastCharacters(2).trim();

        const auto hz = parsing::parseDecimal(freqText);
        if (!hz || *hz <= 0.0)
            return TuningLoadResult::failure("The frequency '" + fields[1] + "' is not a positive number.",
                                             lineNumber);
        if (lastMapped >= 0 && *hz <= table->hz[static_cast<std::size_t>(lastMapped)])
            return TuningLoadResult::failure("Frequencies must increase: row " + juce::String(rows) + " ("
                                                 + juce::String(*hz, 3) + " Hz) is not above row "
                                                 + juce::String(lastMapped) + ".",
                                             lineNumber);

        table->hz[static_cast<std::size_t>(rows)] = *hz;
        table->labels[static_cast<std::size_t>(rows)] = label.isNotEmpty() ? label : labelFor(rows, *hz);
        lastMapped = rows;
        ++rows;
    }

    if (rows == 0)
        return TuningLoadResult::failure("The file has no data rows.");
    if (rows != TuningTable::numNotes)
        return TuningLoadResult::failure("Expected 128 rows, found " + juce::String(rows) + ".");
    if (lastMapped < 0)
        return TuningLoadResult::failure("Every row is unmapped.");

    return TuningLoadResult::success(std::move(table));
}

} // namespace lattice
