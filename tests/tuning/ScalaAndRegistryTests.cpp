#include "tuning/Tet12.h"
#include "tuning/TuningRegistry.h"
#include "tuning/parsers/ScalaParser.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace lattice;
using Catch::Approx;

namespace
{
juce::File dataFile(const char* name)
{
    return juce::File(LATTICE_SOURCE_DIR).getChildFile("tests/data/tunings").getChildFile(name);
}

TuningLoadResult parseScl(const char* scl, const char* kbm = nullptr)
{
    return ScalaParser::parse(dataFile(scl).loadFileAsString(), kbm ? dataFile(kbm).loadFileAsString() : juce::String(),
                              "test", {}, {});
}
} // namespace

TEST_CASE("Scala: a 12-TET .scl matches Tet12", "[tuning][scala]")
{
    const auto r = parseScl("12tet.scl");
    REQUIRE(r.ok());
    const auto tet = Tet12::make();
    for (std::size_t n = 0; n < 128; ++n)
    {
        INFO("note " << n);
        CHECK(r.table->hz[n] == Approx(tet->hz[n]).epsilon(1e-9));
    }
    CHECK(r.table->name == "12-tone equal temperament");
    CHECK(r.table->periodRatio == Approx(2.0));
    CHECK(r.table->keyboardPageSize == 12);
}

TEST_CASE("Scala: a just-intonation scale matches its ratios", "[tuning][scala]")
{
    const auto r = parseScl("ptolemy.scl");
    REQUIRE(r.ok());
    const double c = 261.6255653005986;
    const auto& hz = r.table->hz;
    CHECK(hz[60] == Approx(c));
    CHECK(hz[61] == Approx(c * 9.0 / 8.0));
    CHECK(hz[62] == Approx(c * 5.0 / 4.0));
    CHECK(hz[64] == Approx(c * 3.0 / 2.0));
    CHECK(hz[66] == Approx(c * 15.0 / 8.0));
    CHECK(hz[67] == Approx(c * 2.0));
    CHECK(hz[68] == Approx(c * 2.0 * 9.0 / 8.0));
    CHECK(hz[59] == Approx(c * 15.0 / 16.0));
    CHECK(hz[53] == Approx(c / 2.0));
    CHECK(r.table->keyboardPageSize == 7);
}

TEST_CASE("Scala: a .kbm reference moves the scale", "[tuning][scala]")
{
    const auto r = parseScl("12tet.scl", "a440.kbm");
    REQUIRE(r.ok());
    const auto tet = Tet12::make();
    for (std::size_t n = 0; n < 128; ++n)
        CHECK(r.table->hz[n] == Approx(tet->hz[n]).epsilon(1e-9));
}

TEST_CASE("Scala: .kbm 'x' entries leave notes unmapped", "[tuning][scala]")
{
    const auto r = parseScl("ptolemy.scl", "white-keys.kbm");
    REQUIRE(r.ok());
    const double c = 261.6255653005986;
    const auto& hz = r.table->hz;
    CHECK(hz[60] == Approx(c));
    CHECK(hz[61] == 0.0);             // black key: unmapped
    CHECK(hz[62] == Approx(c * 9.0 / 8.0));
    CHECK(hz[72] == Approx(c * 2.0)); // the map repeats every 12 keys and 7 degrees
    CHECK(r.table->frequencyFor(61.0) == 0.0);
    CHECK(r.table->keyboardPageSize == 12);
}

TEST_CASE("Scala: errors", "[tuning][scala]")
{
    SECTION("count mismatch")
    {
        const auto r = ScalaParser::parse("desc\n 3\n 100.0\n 2/1\n", {}, "t", {}, {});
        CHECK_FALSE(r.ok());
        CHECK(r.error.contains("says it has 3"));
    }
    SECTION("bad pitch, with its line")
    {
        const auto r = ScalaParser::parse("! c\ndesc\n 2\n hello\n 2/1\n", {}, "t", {}, {});
        CHECK_FALSE(r.ok());
        CHECK(r.line == 4);
    }
    SECTION("zero ratio")
    {
        const auto r = ScalaParser::parse("desc\n 2\n 0/1\n 2/1\n", {}, "t", {}, {});
        CHECK_FALSE(r.ok());
        CHECK(r.line == 3);
    }
    SECTION("period not above unison")
    {
        const auto r = ScalaParser::parse("desc\n 1\n -100.0\n", {}, "t", {}, {});
        CHECK_FALSE(r.ok());
    }
    SECTION("blank lines after the description and a trailing newline are ignored")
    {
        const auto r = ScalaParser::parse("\n 2\n\n 3/2\n\n 2/1\n\n", {}, "t", {}, {});
        REQUIRE(r.ok());
        CHECK(r.table->hz[61] == Catch::Approx(261.6255653005986 * 1.5));
    }
    SECTION("negative cents are allowed before the period")
    {
        const auto r = ScalaParser::parse("desc\n 2\n -50.0\n 1200.0\n", {}, "t", {}, {});
        CHECK(r.ok());
    }
}

TEST_CASE("Registry: built-ins, loading, and de-duplication", "[tuning][registry]")
{
    TuningRegistry registry;
    REQUIRE(registry.getAll().size() == 2);
    CHECK(registry.defaultTuning()->id == Tet12::builtinId);
    CHECK(registry.findById(TuningRegistry::colundiId) != nullptr);
    CHECK(registry.findById("nope") == nullptr);

    struct Counter final : TuningRegistry::Listener
    {
        int calls = 0;
        void tuningsChanged() override { ++calls; }
    } counter;
    registry.addListener(&counter);

    const auto first = registry.loadFile(dataFile("ptolemy.scl"));
    REQUIRE(first.ok());
    CHECK(first.table->id.startsWith("user."));
    CHECK(registry.getAll().size() == 3);
    CHECK(counter.calls == 1);

    const auto again = registry.loadFile(dataFile("ptolemy.scl"));
    REQUIRE(again.ok());
    CHECK(again.table == first.table);
    CHECK(registry.getAll().size() == 3);
    CHECK(counter.calls == 1);

    CHECK_FALSE(registry.loadFile(dataFile("a440.kbm")).ok()); // not a scale
    CHECK_FALSE(registry.loadFile(dataFile("missing.scl")).ok());

    const auto colundiCopy = registry.loadFile(juce::File(LATTICE_SOURCE_DIR).getChildFile("resources/tunings/colundi_scale.txt"));
    REQUIRE(colundiCopy.ok());
    CHECK(colundiCopy.table->hz == registry.findById(TuningRegistry::colundiId)->hz);

    registry.removeListener(&counter);
}

TEST_CASE("Registry: a .kbm next to a .scl is used", "[tuning][registry]")
{
    const auto dir = juce::File::createTempFile("lattice-tunings");
    REQUIRE(dir.createDirectory());
    REQUIRE(dataFile("ptolemy.scl").copyFileTo(dir.getChildFile("white.scl")));
    REQUIRE(dataFile("white-keys.kbm").copyFileTo(dir.getChildFile("white.kbm")));
    REQUIRE(dataFile("12tet.scl").copyFileTo(dir.getChildFile("tet.scl")));
    dir.getChildFile("broken.scl").replaceWithText("desc\n 2\n nope\n 2/1\n");

    TuningRegistry registry;
    juce::StringArray errors;
    CHECK(registry.loadFolder(dir, &errors) == 2);
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].startsWith("broken.scl"));

    bool sawUnmapped = false;
    for (const auto& t : registry.getAll())
        if (t->hz[61] == 0.0)
            sawUnmapped = true;
    CHECK(sawUnmapped);

    dir.deleteRecursively();
}
