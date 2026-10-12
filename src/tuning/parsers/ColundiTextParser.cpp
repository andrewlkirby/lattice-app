#include "tuning/parsers/ColundiTextParser.h"

#include "tuning/parsers/NumberParsing.h"

namespace lattice
{

juce::String ColundiTextParser::labelFor(int index, double hz)
{
    static const juce::String dot = juce::String::fromUTF8("\xc2\xb7"); // U+00B7 middle dot
    return "#" + juce::String(index) + " " + dot + " " + juce::String(hz, 2) + " Hz";
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

    for (int i = 0; i < lines.size(); ++i)
    {
        const int lineNumber = i + 1;
        const auto line = lines[i].trimCharactersAtEnd("\r");
        if (line.trim().isEmpty() || line.trimStart().startsWithChar('#'))
            continue;

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

        auto freqText = fields[1];
        if (!freqText.endsWithIgnoreCase("Hz"))
            return TuningLoadResult::failure("The frequency '" + freqText + "' does not end in Hz.", lineNumber);
        freqText = freqText.dropLastCharacters(2).trim();

        const auto hz = parsing::parseDecimal(freqText);
        if (!hz || *hz <= 0.0)
            return TuningLoadResult::failure("The frequency '" + fields[1] + "' is not a positive number.",
                                             lineNumber);
        if (rows > 0 && *hz <= table->hz[static_cast<std::size_t>(rows - 1)])
            return TuningLoadResult::failure("Frequencies must increase: row " + juce::String(rows) + " ("
                                                 + juce::String(*hz, 3) + " Hz) is not above row "
                                                 + juce::String(rows - 1) + ".",
                                             lineNumber);

        table->hz[static_cast<std::size_t>(rows)] = *hz;
        table->labels[static_cast<std::size_t>(rows)] = labelFor(rows, *hz);
        ++rows;
    }

    if (rows == 0)
        return TuningLoadResult::failure("The file has no data rows.");
    if (rows != TuningTable::numNotes)
        return TuningLoadResult::failure("Expected 128 rows, found " + juce::String(rows) + ".");

    return TuningLoadResult::success(std::move(table));
}

} // namespace lattice
