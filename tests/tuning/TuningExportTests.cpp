// Tuning exports (docs/07-m6-plugins-external.md, 6c, "File and sysex export", and "Tests" 5): every
// file reads back to the same 128 frequencies, and MTS sysex bytes are as the MIDI Tuning Standard
// defines them.

#include "tuning/Tet12.h"
#include "tuning/TuningRegistry.h"
#include "tuning/external/TuningExport.h"
#include "tuning/parsers/ScalaParser.h"
#include "tuning/parsers/TunParser.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>

using namespace lattice;
namespace te = lattice::tuning_export;

namespace
{
double centsBetween(double a, double b)
{
    return 1200.0 * std::log2(b / a);
}

/** A user's tuning: Colundi's notes 10..100, with 50 unmapped. */
TuningPtr subset()
{
    auto t = std::make_shared<TuningTable>(*TuningRegistry::makeBuiltinColundi());
    t->id = "test.subset";
    t->name = "Colundi subset";
    for (int n = 0; n < 128; ++n)
        if (n < 10 || n > 100 || n == 50)
            t->hz[static_cast<std::size_t>(n)] = 0.0;
    return t;
}

TuningPtr tableNamed(const juce::String& which)
{
    if (which == "Colundi")
        return TuningRegistry::makeBuiltinColundi();
    if (which == "12-TET")
        return Tet12::make();
    return subset();
}

/** 440 Hz moved by `units` of MTS's 14-bit fraction (100/16384 cents each). */
double unitsFrom440(double units)
{
    return 440.0 * std::pow(2.0, units / (12.0 * 16384.0));
}

using Bytes = std::array<std::uint8_t, 3>;
} // namespace

TEST_CASE("A .tun export reads back to the same frequencies", "[tuning][export]")
{
    const auto which = GENERATE(as<juce::String>{}, "Colundi", "12-TET", "subset");
    INFO(which);
    const auto table = tableNamed(which);
    const auto text = te::toTun(*table);
    CHECK(text.contains("[Exact Tuning]"));
    CHECK(text.contains("[Tuning]"));
    const auto back = TunParser::parse(text, "back", {}, {});
    REQUIRE(back.ok());
    CHECK(back.table->name == table->name);
    for (int n = 0; n < 128; ++n)
    {
        const double hz = table->hz[static_cast<std::size_t>(n)];
        if (hz <= 0.0)
            continue; // .tun can't leave a note out: it's written between its neighbours
        INFO("note " << n);
        CHECK(std::abs(centsBetween(hz, back.table->hz[static_cast<std::size_t>(n)])) < 1e-6);
    }
}

TEST_CASE("A .tun with only whole cents reads as version 1", "[tuning][export]")
{
    const auto back = TunParser::parse("[Tuning]\nnote 69 = 6900\nnote 70 = 7050 ; a quarter tone up\n", "v1", "Old", {});
    REQUIRE(back.ok());
    CHECK(back.table->name == "Old");
    CHECK(back.table->hz[69] == Catch::Approx(440.0).epsilon(1e-9));
    CHECK(back.table->hz[70] == Catch::Approx(440.0 * std::pow(2.0, 1.5 / 12.0)).epsilon(1e-9));
    CHECK(back.table->hz[60] == Catch::Approx(261.6255653).epsilon(1e-8)); // not listed: 12-TET's
    CHECK_FALSE(TunParser::parse("nothing here", "x", {}, {}).ok());
    CHECK_FALSE(TunParser::parse("[Tuning]\nnote 1 = abc\n", "x", {}, {}).ok());
}

TEST_CASE("A .scl and .kbm export reads back to the same 128 frequencies", "[tuning][export]")
{
    const auto which = GENERATE(as<juce::String>{}, "Colundi", "12-TET", "subset");
    INFO(which);
    const auto table = tableNamed(which);
    const auto files = te::toScala(*table);
    REQUIRE(files.has_value());
    const auto back = ScalaParser::parse(files->scl, files->kbm, "back", {}, {});
    REQUIRE(back.ok());
    for (int n = 0; n < 128; ++n)
    {
        INFO("note " << n);
        const double hz = table->hz[static_cast<std::size_t>(n)];
        if (hz <= 0.0)
            CHECK(back.table->hz[static_cast<std::size_t>(n)] == 0.0);
        else
            CHECK(std::abs(centsBetween(hz, back.table->hz[static_cast<std::size_t>(n)])) < 1e-6);
    }
    if (which == "Colundi")
    {
        // 127 steps, the period hz[127]/hz[0].
        const auto scale = ScalaParser::parseScale(files->scl);
        REQUIRE(scale.scale.has_value());
        CHECK(scale.scale->ratios.size() == 127);
        CHECK(scale.scale->ratios.back() == Catch::Approx(table->hz[127] / table->hz[0]).epsilon(1e-9));
    }
}

TEST_CASE("A tuning a Scala scale can't hold isn't exported as one", "[tuning][export]")
{
    auto one = std::make_shared<TuningTable>();
    one->hz[60] = 261.0;
    CHECK_FALSE(te::toScala(*one).has_value());
    auto falling = std::make_shared<TuningTable>();
    falling->hz[10] = 400.0;
    falling->hz[20] = 300.0;
    CHECK_FALSE(te::toScala(*falling).has_value());
}

TEST_CASE("MTS frequency bytes follow the MIDI Tuning Standard", "[tuning][export][mts]")
{
    CHECK(te::mtsBytes(440.0) == Bytes{0x45, 0x00, 0x00});
    CHECK(te::mtsBytes(Tet12::make()->hz[60]) == Bytes{0x3c, 0x00, 0x00});
    CHECK(te::mtsBytes(8.1757989156437073336) == Bytes{0x00, 0x00, 0x00});
    // The 14-bit fraction, rounded to the nearest.
    CHECK(te::mtsBytes(unitsFrom440(1.0)) == Bytes{0x45, 0x00, 0x01});
    CHECK(te::mtsBytes(unitsFrom440(0.49)) == Bytes{0x45, 0x00, 0x00});
    CHECK(te::mtsBytes(unitsFrom440(0.51)) == Bytes{0x45, 0x00, 0x01});
    CHECK(te::mtsBytes(unitsFrom440(128.0)) == Bytes{0x45, 0x01, 0x00});
    CHECK(te::mtsBytes(unitsFrom440(-1.0)) == Bytes{0x44, 0x7f, 0x7f});
    CHECK(te::mtsBytes(unitsFrom440(-0.4)) == Bytes{0x45, 0x00, 0x00}); // rounds up into the next semitone
    // The top: note 127 and 16382/16384 is the highest; 7F 7F 7F is reserved ("no change").
    const double g9 = 440.0 * std::pow(2.0, (127 - 69) / 12.0);
    CHECK(te::mtsBytes(g9) == Bytes{0x7f, 0x00, 0x00});
    CHECK(te::mtsBytes(g9 * std::pow(2.0, 16382.0 / (12.0 * 16384.0))) == Bytes{0x7f, 0x7f, 0x7e});
    CHECK(te::mtsBytes(g9 * std::pow(2.0, 16383.0 / (12.0 * 16384.0))) == Bytes{0x7f, 0x7f, 0x7f});
    CHECK(te::mtsBytes(20000.0) == Bytes{0x7f, 0x7f, 0x7f});
    CHECK(te::mtsBytes(0.0) == Bytes{0x7f, 0x7f, 0x7f});
    CHECK(te::mtsBytes(5.0) == Bytes{0x00, 0x00, 0x00}); // below note 0: note 0
    CHECK(te::hzFromMtsBytes({0x45, 0x00, 0x00}) == Catch::Approx(440.0).epsilon(1e-12));
    CHECK(te::hzFromMtsBytes({0x7f, 0x7f, 0x7f}) == 0.0);

    // Every Colundi note MTS can hold comes back within half a step of the fraction.
    const auto colundi = TuningRegistry::makeBuiltinColundi();
    int held = 0;
    for (int n = 0; n < 128; ++n)
    {
        const double hz = colundi->hz[static_cast<std::size_t>(n)];
        const auto bytes = te::mtsBytes(hz);
        if (bytes == Bytes{0x7f, 0x7f, 0x7f})
        {
            CHECK(hz > 13289.0); // only those above MTS's range
            continue;
        }
        ++held;
        INFO("note " << n);
        CHECK(std::abs(centsBetween(hz, te::hzFromMtsBytes(bytes))) <= 100.0 / 16384.0 / 2.0 + 1e-9);
    }
    CHECK(held > 100);
}

TEST_CASE("An MTS bulk dump has the standard's layout and checksum", "[tuning][export][mts]")
{
    const auto tet = Tet12::make();
    auto named = std::make_shared<TuningTable>(*tet);
    named->name = "Colundi";
    const auto m = te::bulkDump(*named, 5);
    REQUIRE(m.size() == 408);
    CHECK(m[0] == 0xf0);
    CHECK(m[1] == 0x7e);
    CHECK(m[2] == 0x7f);
    CHECK(m[3] == 0x08);
    CHECK(m[4] == 0x01);
    CHECK(m[5] == 5);
    CHECK(juce::String(reinterpret_cast<const char*>(m.data() + 6), 16) == "Colundi         ");
    const std::size_t a4 = 22 + 69 * 3;
    CHECK(m[a4] == 0x45);
    CHECK(m[a4 + 1] == 0x00);
    CHECK(m[a4 + 2] == 0x00);
    std::uint8_t sum = 0;
    for (std::size_t i = 1; i < 406; ++i)
        sum ^= m[i];
    CHECK(m[406] == (sum & 0x7f));
    CHECK(m[407] == 0xf7);
    for (std::size_t i = 1; i < 407; ++i)
        CHECK(m[i] < 0x80);
    CHECK(te::toSyx(m).getSize() == 408);
}

TEST_CASE("MTS single-note changes are split at 127 notes", "[tuning][export][mts]")
{
    const auto colundi = TuningRegistry::makeBuiltinColundi();
    std::vector<int> all(128);
    for (int n = 0; n < 128; ++n)
        all[static_cast<std::size_t>(n)] = n;
    const auto messages = te::noteChanges(*colundi, all);
    REQUIRE(messages.size() == 2);
    CHECK(messages[0].size() == 7 + 127 * 4 + 1);
    CHECK(messages[0][6] == 127);
    CHECK(messages[1].size() == 7 + 4 + 1);
    CHECK(messages[1][6] == 1);
    CHECK(messages[1][7] == 127); // the last note
    const auto& one = messages[0];
    CHECK(one[0] == 0xf0);
    CHECK(one[1] == 0x7f); // real-time
    CHECK(one[3] == 0x08);
    CHECK(one[4] == 0x02);
    CHECK(one.back() == 0xf7);
    const auto a = te::mtsBytes(colundi->hz[47]);
    CHECK(one[7 + 47 * 4] == 47);
    CHECK(one[7 + 47 * 4 + 1] == a[0]);
    CHECK(one[7 + 47 * 4 + 2] == a[1]);
    CHECK(one[7 + 47 * 4 + 3] == a[2]);
}

TEST_CASE("A .tun file loads as a tuning", "[tuning][export]")
{
    const auto file = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("lattice-" + juce::Uuid().toString() + ".tun");
    REQUIRE(file.replaceWithText(te::toTun(*TuningRegistry::makeBuiltinColundi())));
    TuningRegistry registry;
    const auto loaded = registry.loadFile(file);
    file.deleteFile();
    REQUIRE(loaded.ok());
    CHECK(loaded.table->name == "Colundi");
    CHECK(loaded.table->hz[47] == Catch::Approx(TuningRegistry::makeBuiltinColundi()->hz[47]).epsilon(1e-9));
}
