#include "tuning/TuningRegistry.h"
#include "tuning/parsers/ColundiTextParser.h"
#include "tuning/parsers/NumberParsing.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <clocale>
#include <cmath>
#include <cstdlib>
#include <cstring>

using namespace lattice;

namespace
{
juce::File colundiFile()
{
    return juce::File(LATTICE_SOURCE_DIR).getChildFile("resources/tunings/colundi_scale.txt");
}

juce::String colundiText()
{
    return colundiFile().loadFileAsString();
}

TuningLoadResult parseText(const juce::String& text)
{
    ColundiTextParser::Options options;
    options.id = "test.colundi";
    return ColundiTextParser::parse(text, options);
}

/** The file's Freq column, read independently of the parser under test. */
std::vector<double> freqColumn()
{
    juce::StringArray lines;
    lines.addLines(colundiText());
    std::vector<double> out;
    for (const auto& raw : lines)
    {
        const auto line = raw.trimCharactersAtEnd("\r");
        if (line.trim().isEmpty() || line.startsWithChar('#'))
            continue;
        auto fields = juce::StringArray::fromTokens(line, "\t", "");
        fields.remove(0);
        out.push_back(fields[1].upToFirstOccurrenceOf("Hz", false, true).getDoubleValue());
    }
    return out;
}

/** The data lines of the real file, so bad-input tests start from real content. */
juce::StringArray dataLines()
{
    juce::StringArray lines;
    lines.addLines(colundiText());
    lines.removeEmptyStrings();
    return lines;
}
} // namespace

TEST_CASE("Colundi: all 128 rows match the file's Freq column", "[tuning][colundi]")
{
    const auto result = parseText(colundiText());
    REQUIRE(result.ok());
    const auto expected = freqColumn();
    REQUIRE(expected.size() == 128);
    for (std::size_t i = 0; i < expected.size(); ++i)
    {
        INFO("row " << i);
        CHECK(result.table->hz[i] == Catch::Approx(expected[i]).margin(0.001));
    }
}

TEST_CASE("Colundi: spot checks", "[tuning][colundi]")
{
    const auto t = parseText(colundiText()).table;
    REQUIRE(t != nullptr);
    CHECK(t->hz[0] == Catch::Approx(30.0).margin(1e-9));
    CHECK(t->hz[12] == Catch::Approx(59.9).margin(1e-9)); // not 60: the table does not repeat by octave
    CHECK(t->hz[47] == Catch::Approx(440.0).margin(1e-9));
    CHECK(t->hz[119] == Catch::Approx(28160.0).margin(1e-9));
    CHECK(t->hz[127] == Catch::Approx(44098.56).margin(1e-9));
    for (std::size_t i = 1; i < 128; ++i)
        CHECK(t->hz[i] > t->hz[i - 1]);
    CHECK(t->labels[47] == ColundiTextParser::labelFor(47, 440.0));
    CHECK(t->labels[47].startsWith("#47 "));
    CHECK(t->labels[47].endsWith("440.00 Hz"));
    CHECK_FALSE(t->usesNoteNames);
}

TEST_CASE("Colundi: the built-in copy equals the file and has its settings", "[tuning][colundi]")
{
    const auto builtin = TuningRegistry::makeBuiltinColundi();
    const auto fromFile = parseText(colundiText()).table;
    REQUIRE(builtin != nullptr);
    REQUIRE(fromFile != nullptr);
    CHECK(builtin->hz == fromFile->hz);
    CHECK(builtin->id == TuningRegistry::colundiId);
    CHECK(builtin->keyboardPageSize == 17);
    CHECK(builtin->periodRatio == Catch::Approx(938.6666667).margin(1e-6));
}

TEST_CASE("Colundi: rows with scientific-notation cents and comma names parse", "[tuning][colundi]")
{
    // Rows 101-127 have cents like 1.013e+4, and every Name uses a comma decimal.
    const auto lines = dataLines();
    REQUIRE(lines.joinIntoString("\n").contains("1.013e+4"));
    REQUIRE(lines.joinIntoString("\n").contains("1,06666"));
    const auto result = parseText(lines.joinIntoString("\n"));
    REQUIRE(result.ok());
    CHECK(result.table->hz[101] == Catch::Approx(10402.0));
}

TEST_CASE("Colundi: bad input reports the error and line", "[tuning][colundi]")
{
    SECTION("empty file")
    {
        const auto r = parseText("");
        CHECK_FALSE(r.ok());
        CHECK(r.error.containsIgnoreCase("empty"));
    }

    SECTION("127 rows instead of 128")
    {
        auto lines = dataLines();
        lines.remove(lines.size() - 1);
        const auto r = parseText(lines.joinIntoString("\n"));
        CHECK_FALSE(r.ok());
        CHECK(r.error.contains("Expected 128 rows, found 127"));
        CHECK(r.line == 0);
    }

    SECTION("129 rows")
    {
        auto lines = dataLines();
        lines.add("\t128\t50000.000Hz\t0\t0\t0");
        const auto r = parseText(lines.joinIntoString("\n"));
        CHECK_FALSE(r.ok());
        CHECK(r.error.contains("More than 128"));
        CHECK(r.line == lines.size());
    }

    SECTION("a row number that isn't a number")
    {
        auto lines = dataLines(); // line 1 is the header, so row 5 is on line 7
        lines.set(6, lines[6].replace("\t5\t", "\tfive\t"));
        const auto r = parseText(lines.joinIntoString("\n"));
        CHECK_FALSE(r.ok());
        CHECK(r.line == 7);
        CHECK(r.error.contains("five"));
    }

    SECTION("a frequency that isn't a number")
    {
        auto lines = dataLines();
        REQUIRE(lines[11].contains("52.500Hz"));
        lines.set(11, lines[11].replace("52.500Hz", "5Z.500Hz"));
        const auto r = parseText(lines.joinIntoString("\n"));
        CHECK_FALSE(r.ok());
        CHECK(r.line == 12);
    }

    SECTION("a comma decimal in the frequency is rejected, not misread")
    {
        auto lines = dataLines();
        lines.set(48, lines[48].replace("440.000Hz", "440,000Hz"));
        const auto r = parseText(lines.joinIntoString("\n"));
        CHECK_FALSE(r.ok());
        CHECK(r.line == 49);
    }

    SECTION("a non-increasing frequency")
    {
        auto lines = dataLines();
        lines.set(49, lines[49].replace("480.000Hz", "430.000Hz")); // row 48 below row 47 (440)
        const auto r = parseText(lines.joinIntoString("\n"));
        CHECK_FALSE(r.ok());
        CHECK(r.line == 50);
        CHECK(r.error.contains("increase"));
    }

    SECTION("a frequency without Hz")
    {
        auto lines = dataLines();
        lines.set(3, lines[3].replace("Hz", ""));
        const auto r = parseText(lines.joinIntoString("\n"));
        CHECK_FALSE(r.ok());
        CHECK(r.line == 4);
        CHECK(r.error.contains("Hz"));
    }
}

TEST_CASE("Colundi: parsing is identical under a comma-decimal locale", "[tuning][colundi][locale]")
{
    const auto before = parseText(colundiText()).table;
    REQUIRE(before != nullptr);

    const std::string previous = std::setlocale(LC_ALL, nullptr);
    const char* candidates[] = {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "German_Germany.1252", "de-DE"};
    const char* active = nullptr;
    for (const char* name : candidates)
    {
        if (std::setlocale(LC_ALL, name) != nullptr && std::strcmp(std::localeconv()->decimal_point, ",") == 0)
        {
            active = name;
            break;
        }
    }
    if (active == nullptr)
    {
        std::setlocale(LC_ALL, previous.c_str());
        SKIP("No comma-decimal locale is installed");
    }

    // Prove the locale really is in effect: the C library now stops at the '.'.
    CHECK(std::strtod("1.5", nullptr) == 1.0);

    const auto after = parseText(colundiText()).table;
    std::setlocale(LC_ALL, previous.c_str());

    INFO("locale " << active);
    REQUIRE(after != nullptr);
    CHECK(after->hz == before->hz);
}

TEST_CASE("Number parsing is strict", "[tuning][parsing]")
{
    using parsing::parseDecimal;
    CHECK(parseDecimal("440.000") == 440.0);
    CHECK(parseDecimal("1.013e+4") == 10130.0);
    CHECK(parseDecimal("-12.5") == -12.5);
    CHECK(parseDecimal(".5") == 0.5);
    CHECK_FALSE(parseDecimal("1,5").has_value());
    CHECK_FALSE(parseDecimal("").has_value());
    CHECK_FALSE(parseDecimal("1.5x").has_value());
    CHECK_FALSE(parseDecimal(" 1.5").has_value());
    CHECK_FALSE(parseDecimal("1e").has_value());
    CHECK_FALSE(parseDecimal(".").has_value());
    CHECK(parsing::parseInteger("127") == 127);
    CHECK_FALSE(parsing::parseInteger("-1").has_value());
    CHECK_FALSE(parsing::parseInteger("12a").has_value());
}
