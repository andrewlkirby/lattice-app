#include "tuning/Tet12.h"
#include "tuning/TuningEdits.h"
#include "tuning/TuningRegistry.h"
#include "tuning/parsers/ColundiTextParser.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

using namespace lattice;
using Catch::Approx;

namespace
{
TuningLoadResult reparse(const TuningTable& table)
{
    ColundiTextParser::Options options;
    options.id = "test.written";
    options.name = table.name;
    return ColundiTextParser::parse(ColundiTextParser::write(table), options);
}

void requireSameTable(const TuningTable& a, const TuningTable& b)
{
    for (int i = 0; i < TuningTable::numNotes; ++i)
    {
        INFO("entry " << i);
        CHECK(b.hz[static_cast<std::size_t>(i)] == Approx(a.hz[static_cast<std::size_t>(i)]).epsilon(1e-9));
        CHECK(b.labels[static_cast<std::size_t>(i)] == a.labels[static_cast<std::size_t>(i)]);
    }
    CHECK(b.periodRatio == Approx(a.periodRatio).epsilon(1e-9));
    CHECK(b.keyboardPageSize == a.keyboardPageSize);
    CHECK(b.usesNoteNames == a.usesNoteNames);
}
} // namespace

TEST_CASE("Tuning files: what the editor writes reads back the same", "[tuning][edits]")
{
    SECTION("Colundi: its period, page size and every frequency")
    {
        const auto colundi = TuningRegistry::makeBuiltinColundi();
        const auto back = reparse(*colundi);
        REQUIRE(back.ok());
        requireSameTable(*colundi, *back.table);
        CHECK(back.table->isAnchor(1) == colundi->isAnchor(1));
        for (int i = 0; i < TuningTable::numNotes; ++i)
            CHECK(back.table->isAnchor(i) == colundi->isAnchor(i)); // the anchor test is to 0.0005 Hz
    }
    SECTION("12-TET: note names as labels, and frequencies with more than three decimals")
    {
        const auto tet = Tet12::make(440.0);
        const auto back = reparse(*tet);
        REQUIRE(back.ok());
        requireSameTable(*tet, *back.table);
        CHECK(back.table->labels[60] == "C4");
    }
    SECTION("labels of its own, and unmapped entries")
    {
        auto t = *TuningRegistry::makeBuiltinColundi();
        t.labels[47] = "A (the 440)";
        t = tuning_edits::shifted(t, 3);
        const auto back = reparse(t);
        REQUIRE(back.ok());
        requireSameTable(t, *back.table);
        CHECK(back.table->hz[0] == 0.0);
        CHECK(back.table->labels[0] == "#0 (unmapped)");
        CHECK(back.table->labels[50] == "A (the 440)");
        CHECK(back.table->labels[51] == ColundiTextParser::labelFor(51, t.hz[51]));
    }
}

TEST_CASE("Tuning files: the format's additions", "[tuning][edits]")
{
    juce::String text = "# Period: 3\n# Keyboard page: 7\n#\tFreq\tCents\tRatio\tName\tLabel\n";
    for (int i = 0; i < 128; ++i)
        text << "\t" << i << "\t" << (i % 2 == 0 ? juce::String(100.0 + i, 3) + "Hz" : juce::String("-")) << "\t\t\t\t"
             << (i == 4 ? "four" : "") << "\n";
    ColundiTextParser::Options options;
    options.id = "test";
    const auto r = ColundiTextParser::parse(text, options);
    REQUIRE(r.ok());
    CHECK(r.table->periodRatio == 3.0);
    CHECK(r.table->keyboardPageSize == 7);
    CHECK(r.table->hz[1] == 0.0);
    CHECK(r.table->hz[2] == Approx(102.0));
    CHECK(r.table->labels[4] == "four");
    CHECK(r.table->labels[2] == ColundiTextParser::labelFor(2, 102.0));

    SECTION("frequencies must still rise across unmapped rows")
    {
        auto bad = text.replace("\t4\t104.000Hz", "\t4\t99.000Hz");
        const auto failed = ColundiTextParser::parse(bad, options);
        CHECK(!failed.ok());
        CHECK(failed.error.contains("row 2"));
    }
    SECTION("a period that isn't above 1 is refused")
    {
        CHECK(!ColundiTextParser::parse(text.replace("# Period: 3", "# Period: 1"), options).ok());
    }
    SECTION("all rows unmapped is refused")
    {
        juce::String none;
        for (int i = 0; i < 128; ++i)
            none << "\t" << i << "\t-\n";
        CHECK(!ColundiTextParser::parse(none, options).ok());
    }
}

TEST_CASE("Tuning edits: shift, multiply, subset", "[tuning][edits]")
{
    const auto colundi = TuningRegistry::makeBuiltinColundi();

    SECTION("shift up by 5: each entry 5 keys higher, the bottom 5 unmapped, the top 5 dropped")
    {
        const auto t = tuning_edits::shifted(*colundi, 5);
        for (int i = 0; i < 5; ++i)
            CHECK(t.hz[static_cast<std::size_t>(i)] == 0.0);
        CHECK(t.hz[52] == colundi->hz[47]);
        CHECK(t.hz[127] == colundi->hz[122]);
        CHECK(t.labels[52] == ColundiTextParser::labelFor(52, 440.0));
        CHECK(t.periodRatio == colundi->periodRatio);
        CHECK(tuning_edits::check(t, 48000.0).ok());
    }
    SECTION("shift down")
    {
        const auto t = tuning_edits::shifted(*colundi, -2);
        CHECK(t.hz[45] == colundi->hz[47]);
        CHECK(t.hz[126] == 0.0);
        CHECK(t.hz[127] == 0.0);
    }
    SECTION("multiply by 3/2")
    {
        const auto ratio = tuning_edits::parseRatio("3/2");
        REQUIRE(ratio);
        const auto t = tuning_edits::multiplied(*colundi, *ratio);
        CHECK(t.hz[47] == Approx(660.0));
        CHECK(t.hz[0] == Approx(45.0));
    }
    SECTION("subset: the anchors on consecutive keys, centred on key 60")
    {
        const auto anchors = tuning_edits::anchorIndices(*colundi);
        REQUIRE(anchors.size() > 4);
        CHECK(anchors.front() == 0); // 30 Hz, the base
        const int start = tuning_edits::centredStart(static_cast<int>(anchors.size()));
        const auto t = tuning_edits::subset(*colundi, anchors, start);
        CHECK(t.hz[static_cast<std::size_t>(start)] == colundi->hz[0]);
        CHECK(t.hz[static_cast<std::size_t>(start + 1)] == colundi->hz[static_cast<std::size_t>(anchors[1])]);
        CHECK(t.hz[static_cast<std::size_t>(start - 1)] == 0.0);
        int mapped = 0;
        for (const double hz : t.hz)
            mapped += hz > 0.0 ? 1 : 0;
        CHECK(mapped == static_cast<int>(anchors.size()));
        CHECK(tuning_edits::check(t, 0.0).ok());
    }
    SECTION("subset: order and duplicates don't matter, and keys past 127 are dropped")
    {
        const auto t = tuning_edits::subset(*colundi, {50, 47, 47, 53}, 126);
        CHECK(t.hz[126] == colundi->hz[47]);
        CHECK(t.hz[127] == colundi->hz[50]);
    }
}

TEST_CASE("Tuning edits: ratios as typed", "[tuning][edits]")
{
    CHECK(*tuning_edits::parseRatio("3/2") == Approx(1.5));
    CHECK(*tuning_edits::parseRatio(" 1.25 ") == Approx(1.25));
    CHECK(*tuning_edits::parseRatio("1200c") == Approx(2.0));
    CHECK(*tuning_edits::parseRatio("-50 cents") == Approx(std::exp2(-50.0 / 1200.0)));
    CHECK(*tuning_edits::parseRatio("+700c") == Approx(std::exp2(700.0 / 1200.0)));
    CHECK(!tuning_edits::parseRatio("0"));
    CHECK(!tuning_edits::parseRatio("-2"));
    CHECK(!tuning_edits::parseRatio("1/0"));
    CHECK(!tuning_edits::parseRatio("1,5"));
    CHECK(!tuning_edits::parseRatio("abc"));
}

TEST_CASE("Tuning edits: the checks", "[tuning][edits]")
{
    auto t = *TuningRegistry::makeBuiltinColundi();
    SECTION("Colundi at 48 kHz: its top entries are above the playable limit (warnings, not errors)")
    {
        const auto c = tuning_edits::check(t, 48000.0);
        CHECK(c.ok());
        int above = 0;
        for (const double hz : t.hz)
            above += hz >= 48000.0 * 0.45 ? 1 : 0;
        CHECK(c.aboveLimit == above);
        CHECK(above > 0);
        CHECK(c.problems.front().row == 128 - above);
    }
    SECTION("a frequency that doesn't rise is an error on its row")
    {
        t.hz[47] = t.hz[46];
        const auto c = tuning_edits::check(t, 0.0);
        CHECK(!c.ok());
        REQUIRE(c.problems.size() == 1);
        CHECK(c.problems[0].row == 47);
        CHECK(c.problems[0].error);
    }
    SECTION("nothing mapped")
    {
        t.hz.fill(0.0);
        CHECK(!tuning_edits::check(t, 0.0).ok());
    }
}

TEST_CASE("Tuning registry: a file saved again replaces its older version in the list", "[tuning][edits]")
{
    const auto folder = juce::File::createTempFile("tunings");
    folder.createDirectory();
    const auto file = folder.getChildFile("Mine.txt");
    auto table = *TuningRegistry::makeBuiltinColundi();
    table.name = "Mine";
    REQUIRE(file.replaceWithText(ColundiTextParser::write(table)));

    TuningRegistry registry;
    const auto before = registry.getAll().size();
    const auto first = registry.loadFile(file);
    REQUIRE(first.ok());
    CHECK(registry.getAll().size() == before + 1);

    REQUIRE(file.replaceWithText(ColundiTextParser::write(tuning_edits::multiplied(table, 2.0))));
    const auto second = registry.loadFile(file);
    REQUIRE(second.ok());
    CHECK(second.table->id != first.table->id);
    CHECK(registry.getAll().size() == before + 1);
    CHECK(registry.getAll().back() == second.table);
    CHECK(registry.findById(first.table->id) == first.table); // sets may still name it

    // Back to the first version: it comes back to the list.
    REQUIRE(file.replaceWithText(ColundiTextParser::write(table)));
    CHECK(registry.loadFile(file).table == first.table);
    CHECK(registry.getAll().size() == before + 1);
    CHECK(registry.getAll().back() == first.table);
    folder.deleteRecursively();
}
