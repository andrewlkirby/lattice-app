// Lattice as the MTS-ESP master (docs/07-m6-plugins-external.md, 6c, and "Tests" 4): a client reads
// back exactly the frequencies sent, multi-channel mode gives each tuning its own channel, and
// notes a tuning doesn't play are filtered.
//
// The SDK's own copy of libMTS is used, so these run whether MTS-ESP is installed or not. There's
// only one master on a machine (libMTS shares it between processes), so the tests take turns
// across processes; if something else is the master (Lattice itself, or another program), they
// say so and stop.

#include "support/MtsEspSupport.h"
#include "tuning/Tet12.h"
#include "tuning/TrackTuning.h"
#include "tuning/TuningRegistry.h"

#include <catch2/catch_test_macros.hpp>

using namespace lattice;

namespace
{
mts::Tuning tableOf(const TuningTable& t)
{
    mts::Tuning tuning;
    tuning.hz = t.hz;
    tuning.name = t.name;
    tuning.periodRatio = t.periodRatio;
    return tuning;
}

/** What a track plays: the table at its sample rate, with what's out of range filtered. */
mts::Tuning played(TuningPtr table, double sampleRate, int offset = 0)
{
    TrackTuning t(std::move(table));
    t.setSampleRate(sampleRate);
    t.setNoteOffset(offset);
    mts::Tuning tuning;
    for (int n = 0; n < 128; ++n)
        tuning.hz[static_cast<std::size_t>(n)] = t.hzFor(n);
    tuning.name = t.getTuning()->name;
    tuning.periodRatio = t.getTuning()->periodRatio;
    return tuning;
}

/** A user's tuning: 12-TET a quarter tone up. */
mts::Tuning quarterUp()
{
    auto t = tableOf(*Tet12::make());
    for (auto& hz : t.hz)
        hz *= std::pow(2.0, 0.5 / 12.0);
    t.name = "Quarter up";
    return t;
}

/** The global table as a client sees it. */
void checkGlobal(const mts::Library& library, const mts::Tuning& sent)
{
    const double* table = library.tuningTable();
    REQUIRE(table != nullptr);
    for (int n = 0; n < 128; ++n)
    {
        INFO("note " << n);
        const double hz = sent.hz[static_cast<std::size_t>(n)];
        CHECK(library.shouldFilterNote(n, -1) == (hz <= 0.0));
        if (hz > 0.0)
            CHECK(table[n] == hz); // exactly: doubles all the way
    }
}

void checkChannel(const mts::Library& library, int channel, const mts::Tuning& sent)
{
    REQUIRE(library.usesMultiChannelTuning(channel));
    const double* table = library.multiChannelTuningTable(channel);
    REQUIRE(table != nullptr);
    for (int n = 0; n < 128; ++n)
    {
        INFO("channel " << channel << ", note " << n);
        const double hz = sent.hz[static_cast<std::size_t>(n)];
        CHECK(library.shouldFilterNoteMultiChannel(n, channel) == (hz <= 0.0));
        if (hz > 0.0)
            CHECK(table[n] == hz);
    }
}
} // namespace

TEST_CASE("MTS-ESP: a client reads back exactly what the master sends", "[tuning][mts]")
{
    test::MtsEspTurn turn;
    if (!turn.ours())
        return;
    mts::Master master(test::mtsLibraryFile());
    master.setEnabled(true);
    REQUIRE(master.status() == mts::Master::Status::active);
    auto& library = turn.library();
    library.registerClient();
    CHECK(master.clients() == 1);

    SECTION("Colundi, with what's above 0.45 of the sample rate filtered")
    {
        const auto colundi = played(TuningRegistry::makeBuiltinColundi(), 48000.0);
        int filtered = 0;
        for (const double hz : colundi.hz)
            filtered += hz <= 0.0 ? 1 : 0;
        CHECK(filtered > 0);
        CHECK(master.send({colundi}) == std::vector<int>{-1});
        checkGlobal(library, colundi);
        CHECK(library.scaleName() == "Colundi");
        CHECK_FALSE(library.usesMultiChannelTuning(0));
    }
    SECTION("12-TET")
    {
        const auto tet = played(Tet12::make(), 48000.0);
        master.send({tet});
        checkGlobal(library, tet);
    }
    SECTION("A user's tuning, and a track's note offset")
    {
        const auto user = quarterUp();
        master.send({user});
        checkGlobal(library, user);
        const auto shifted = played(TuningRegistry::makeBuiltinColundi(), 48000.0, 5);
        master.send({shifted});
        checkGlobal(library, shifted);
        CHECK(library.tuningTable()[0] == TuningRegistry::makeBuiltinColundi()->hz[5]);
    }
    library.deregisterClient();
}

TEST_CASE("MTS-ESP: different tunings each get a channel", "[tuning][mts]")
{
    test::MtsEspTurn turn;
    if (!turn.ours())
        return;
    mts::Master master(test::mtsLibraryFile());
    master.setEnabled(true);
    REQUIRE(master.status() == mts::Master::Status::active);
    auto& library = turn.library();
    const auto colundi = played(TuningRegistry::makeBuiltinColundi(), 48000.0);
    const auto tet = played(Tet12::make(), 48000.0);
    const auto user = quarterUp();

    // Three tracks, two tunings.
    CHECK(master.send({colundi, tet, colundi}) == std::vector<int>{0, 1, 0});
    checkChannel(library, 0, colundi);
    checkChannel(library, 1, tet);
    CHECK_FALSE(library.usesMultiChannelTuning(2));
    checkGlobal(library, colundi); // for clients that don't ask by channel

    // A tuning keeps its channel while it's in use; a new one takes the free one.
    CHECK(master.send({tet, user}) == std::vector<int>{1, 0});
    checkChannel(library, 0, user);
    checkChannel(library, 1, tet);

    // Back to one tuning: no channels.
    CHECK(master.send({user, user}) == std::vector<int>{-1, -1});
    CHECK_FALSE(library.usesMultiChannelTuning(0));
    CHECK_FALSE(library.usesMultiChannelTuning(1));
    checkGlobal(library, user);

    // More than 16: the rest get none, and it says so.
    std::vector<mts::Tuning> many;
    for (int i = 0; i < 17; ++i)
    {
        auto t = tet;
        for (auto& hz : t.hz)
            hz *= 1.0 + i / 1000.0;
        many.push_back(t);
    }
    const auto channels = master.send(many);
    CHECK(channels[15] == 15);
    CHECK(channels[16] == -1);
    CHECK(master.withoutChannel() == 1);
    CHECK(master.statusText().contains("16"));
}

TEST_CASE("MTS-ESP: another master is left alone, and Lattice takes over when it goes", "[tuning][mts]")
{
    test::MtsEspTurn turn;
    if (!turn.ours())
        return;
    auto& library = turn.library();
    library.registerMaster(); // another program
    mts::Master master(test::mtsLibraryFile());
    master.setEnabled(true);
    CHECK(master.status() == mts::Master::Status::otherMaster);
    CHECK(master.statusText().contains("Another program"));
    const auto tet = played(Tet12::make(), 48000.0);
    master.send({tet}); // kept for later, not sent
    library.deregisterMaster();
    master.retry();
    CHECK(master.status() == mts::Master::Status::active);
    checkGlobal(library, tet);

    master.setEnabled(false);
    CHECK(master.status() == mts::Master::Status::off);
    CHECK_FALSE(library.hasMaster());
}

TEST_CASE("MTS-ESP: without the library, the master says how to get it", "[tuning][mts]")
{
    mts::Master master(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("no-such-libMTS.dll"));
    master.setEnabled(true);
    CHECK(master.status() == mts::Master::Status::noLibrary);
    CHECK(master.statusText().contains("MTS-ESP Mini"));
    CHECK(master.send({played(Tet12::make(), 48000.0)}) == std::vector<int>{-1});
}
