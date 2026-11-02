// A track's tuning settings reach the engine (docs/05-m4-ui.md, 4f): pitch bend in cents or table
// steps with its range, and the tuning editor's draft played in place of the track's tuning.

#include "engine/SessionController.h"
#include "support/TestDevices.h"
#include "tuning/TuningEdits.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

using namespace lattice;
using namespace lattice::test;
using Catch::Approx;

namespace
{
struct Fixture
{
    Fixture() { session.newSet(2, 1); }
    TrackTuning& tuning(int track) { return session.processorForTrack(track)->tuning(); }

    TuningRegistry tunings;
    AudioEngine engine;
    SessionController session{engine, tunings, testRegistry()};
};
} // namespace

TEST_CASE("Track tuning: the bend settings reach the track's tuning", "[engine][tuning]")
{
    Fixture f;
    auto& set = f.session.set();
    f.session.flush();
    CHECK(f.tuning(0).getBendMode() == BendMode::Cents);
    double steps = 0.0, cents = 0.0;
    f.tuning(0).bendOffsets(1.0f, steps, cents);
    CHECK(cents == Approx(LiveSet::defaultBendRangeCents));

    set.setTrackBendMode(0, 1);
    set.setTrackBendRangeSteps(0, 5.0);
    set.setTrackBendRangeCents(1, 1200.0);
    f.session.flush();
    CHECK(f.tuning(0).getBendMode() == BendMode::TableSteps);
    f.tuning(0).bendOffsets(0.5f, steps, cents);
    CHECK(steps == Approx(2.5));
    CHECK(cents == 0.0);
    f.tuning(1).bendOffsets(-1.0f, steps, cents);
    CHECK(cents == Approx(-1200.0));

    // The ranges are kept within their limits.
    set.setTrackBendRangeSteps(0, 1000.0);
    CHECK(LiveSet::bendRangeStepsOf(set.track(0)) == LiveSet::maxBendRangeSteps);
    set.setTrackBendRangeCents(0, -5.0);
    CHECK(LiveSet::bendRangeCentsOf(set.track(0)) == 0.0);
}

TEST_CASE("Track tuning: a set saved before the bend settings has the defaults", "[engine][tuning]")
{
    Fixture f;
    auto track = f.session.set().track(0);
    for (const auto& id : {ids::bendMode, ids::bendRangeCents, ids::bendRangeSteps})
        track.removeProperty(id, nullptr);
    CHECK(LiveSet::bendModeOf(track) == 0);
    CHECK(LiveSet::bendRangeCentsOf(track) == LiveSet::defaultBendRangeCents);
    CHECK(LiveSet::bendRangeStepsOf(track) == LiveSet::defaultBendRangeSteps);
}

TEST_CASE("Track tuning: a draft plays in place of the track's tuning until it's cleared", "[engine][tuning]")
{
    Fixture f;
    auto& set = f.session.set();
    set.setTrackTuning(0, TuningRegistry::colundiId);
    f.session.flush();
    const auto colundi = f.tunings.findById(TuningRegistry::colundiId);
    CHECK(f.tuning(0).getTuning() == colundi);

    auto draft = std::make_shared<TuningTable>(tuning_edits::multiplied(*colundi, 2.0));
    f.session.snapshots().setTuningOverride(set.trackId(0), draft);
    f.session.flush();
    CHECK(f.tuning(0).getTuning() == draft);
    CHECK(f.tuning(0).hzFor(47) == Approx(880.0));
    CHECK(f.tuning(1).getTuning() != draft); // only that track

    f.session.snapshots().setTuningOverride(set.trackId(0), nullptr);
    f.session.flush();
    CHECK(f.tuning(0).getTuning() == colundi);
}
