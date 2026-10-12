#include "core/RealtimeCheck.h"
#include "tuning/Tet12.h"
#include "tuning/TrackTuning.h"
#include "tuning/TuningRegistry.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

using namespace lattice;
using Catch::Approx;

TEST_CASE("Interpolation between table entries is logarithmic and clamped", "[tuning]")
{
    const auto t = TuningRegistry::makeBuiltinColundi();
    REQUIRE(t != nullptr);
    CHECK(t->frequencyFor(47.0) == Approx(440.0));
    CHECK(t->frequencyFor(47.5) == Approx(std::sqrt(t->hz[47] * t->hz[48])));
    CHECK(t->frequencyFor(47.25) == Approx(t->hz[47] * std::pow(t->hz[48] / t->hz[47], 0.25)));
    CHECK(t->frequencyFor(-3.0) == Approx(t->hz[0]));
    CHECK(t->frequencyFor(127.0) == Approx(t->hz[127]));
    CHECK(t->frequencyFor(200.0) == Approx(t->hz[127]));
}

TEST_CASE("12-TET table", "[tuning]")
{
    const auto t = Tet12::make();
    CHECK(t->hz[69] == Approx(440.0));
    CHECK(t->hz[60] == Approx(261.6255653005986));
    CHECK(t->hz[81] == Approx(880.0));
    CHECK(t->labels[60] == "C4");
    CHECK(t->labels[69] == "A4");
    CHECK(t->usesNoteNames);
    CHECK(t->defaultKeyboardBase() == 60);
    CHECK(Tet12::make(432.0)->hz[69] == Approx(432.0));
}

TEST_CASE("Colundi's default keyboard page holds the note nearest middle C", "[tuning]")
{
    const auto t = TuningRegistry::makeBuiltinColundi();
    const int base = t->defaultKeyboardBase();
    CHECK(base % 17 == 0);
    const int middle = t->nearestNoteTo(261.6255653005986);
    CHECK(middle >= base);
    CHECK(middle < base + 17);
}

TEST_CASE("Bend modes", "[tuning]")
{
    SECTION("cents: +200 cents on A4")
    {
        TrackTuning tt(Tet12::make());
        tt.setBendMode(BendMode::Cents);
        tt.setBendRangeCents(200.0f);
        CHECK(tt.hzFor(69, 1.0f) == Approx(493.8833).margin(0.001));
        CHECK(tt.hzFor(69, -1.0f) == Approx(391.9954).margin(0.001));
        CHECK(tt.hzFor(69, 0.0f) == Approx(440.0));
    }

    SECTION("table steps: +1 step on Colundi note 47 lands on entry 48")
    {
        const auto colundi = TuningRegistry::makeBuiltinColundi();
        TrackTuning tt(colundi);
        tt.setBendMode(BendMode::TableSteps);
        tt.setBendRangeSteps(2.0f);
        CHECK(tt.hzFor(47, 0.5f) == Approx(colundi->hz[48]));
        CHECK(tt.hzFor(47, -1.0f) == Approx(colundi->hz[45]));
        CHECK(tt.hzFor(47, 0.25f) == Approx(std::sqrt(colundi->hz[47] * colundi->hz[48])));
    }

    SECTION("note offset shifts the table")
    {
        TrackTuning tt(Tet12::make());
        tt.setNoteOffset(12);
        CHECK(tt.hzFor(57) == Approx(440.0));
        CHECK(tt.hzFor(120) == 0.0); // 132 is off the table
    }
}

TEST_CASE("High-note policy", "[tuning]")
{
    const auto colundi = TuningRegistry::makeBuiltinColundi();
    TrackTuning tt(colundi);
    tt.setSampleRate(48000.0); // limit 21.6 kHz: entries 115-127 are out of range

    SECTION("Mute")
    {
        tt.setHighNotePolicy(HighNotePolicy::Mute);
        CHECK(tt.hzFor(114) == Approx(colundi->hz[114]));
        CHECK(tt.hzFor(115) == 0.0);
        CHECK(tt.hzFor(127) == 0.0);
    }

    SECTION("FoldDown divides by the period until in range")
    {
        tt.setHighNotePolicy(HighNotePolicy::FoldDown);
        const double hz = tt.hzFor(127);
        CHECK(hz > 0.0);
        CHECK(hz < 0.45 * 48000.0);
        CHECK(hz == Approx(colundi->hz[127] / TuningRegistry::colundiPeriodRatio));
    }

    SECTION("no sample rate yet: no policy")
    {
        TrackTuning fresh(colundi);
        CHECK(fresh.hzFor(127) == Approx(colundi->hz[127]));
    }
}

TEST_CASE("Unmapped or missing tunings are silent", "[tuning]")
{
    TrackTuning empty;
    CHECK(empty.hzFor(60) == 0.0);
    TrackTuning tt(Tet12::make());
    CHECK(tt.hzFor(-1) == 0.0);
    CHECK(tt.hzFor(128) == 0.0);
}

TEST_CASE("Swapping tunings while readers run is safe and frees nothing on the readers",
          "[tuning][rt][stress]")
{
    // Tagged [stress]: the long version runs nightly. A short run is in the next test.
    const auto a = Tet12::make();
    const auto b = TuningRegistry::makeBuiltinColundi();
    TrackTuning tt(a);
    const double validA = a->hz[47], validB = b->hz[47];

    std::atomic<bool> stop{false};
    std::atomic<int> badValues{0};
    const auto violationsBefore = rt::violationCount();

    std::vector<std::thread> readers;
    readers.reserve(2);
    for (int r = 0; r < 2; ++r)
    {
        readers.emplace_back([&] {
            const rt::ScopedAudioThread audioThread;
            while (!stop.load())
            {
                const double hz = tt.hzFor(47);
                if (hz != validA && hz != validB)
                    badValues.fetch_add(1);
            }
        });
    }

    for (int i = 0; i < 200000; ++i)
    {
        tt.setTuning(i % 2 == 0 ? b : a);
        tt.collectGarbage();
    }
    stop.store(true);
    for (auto& t : readers)
        t.join();
    tt.collectGarbage();

    CHECK(badValues.load() == 0);
    CHECK(rt::violationCount() == violationsBefore);
    CHECK(tt.pendingGarbage() == 0);
}

TEST_CASE("Swapping tunings while a reader runs (short)", "[tuning][rt]")
{
    const auto a = Tet12::make();
    const auto b = TuningRegistry::makeBuiltinColundi();
    TrackTuning tt(a);
    std::atomic<bool> stop{false};
    std::atomic<int> badValues{0};
    const auto violationsBefore = rt::violationCount();

    std::thread reader([&] {
        const rt::ScopedAudioThread audioThread;
        while (!stop.load())
        {
            const double hz = tt.hzFor(47);
            if (hz != a->hz[47] && hz != b->hz[47])
                badValues.fetch_add(1);
        }
    });
    for (int i = 0; i < 5000; ++i)
        tt.setTuning(i % 2 == 0 ? b : a);
    stop.store(true);
    reader.join();
    tt.collectGarbage();

    CHECK(badValues.load() == 0);
    CHECK(rt::violationCount() == violationsBefore);
    CHECK(tt.pendingGarbage() == 0);
}
