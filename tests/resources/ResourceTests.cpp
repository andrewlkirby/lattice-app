#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

TEST_CASE("The Colundi table is present for M1 with 128 rows", "[resources]")
{
    const auto file = juce::File(LATTICE_SOURCE_DIR).getChildFile("resources/tunings/colundi_scale.txt");
    REQUIRE(file.existsAsFile());

    juce::StringArray lines;
    lines.addLines(file.loadFileAsString());
    int rows = 0;
    for (const auto& line : lines)
    {
        const auto trimmed = line.trim();
        if (trimmed.isNotEmpty() && juce::CharacterFunctions::isDigit(trimmed[0]))
            ++rows;
    }
    CHECK(rows == 128);
}
