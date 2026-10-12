#include "core/RealtimeCheck.h"
#include "engine/AudioEngine.h"
#include "support/AllocationHooks.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <vector>

using namespace lattice;

TEST_CASE("Rendering 10,000 blocks allocates nothing", "[engine][rt]")
{
    LATTICE_REQUIRE_ALLOCATION_HOOKS();

    AudioEngine engine;
    engine.prepare(48000.0, 256);
    engine.testTone().setEnabled(true);

    std::vector<float> left(256), right(256);
    std::array<float*, 2> outputs{left.data(), right.data()};

    const auto before = rt::countersForThisThread();
    const auto violationsBefore = rt::violationCount();
    {
        const rt::ScopedAudioThread audioThread;
        for (int i = 0; i < 10000; ++i)
        {
            engine.process(outputs.data(), 2, 256);
            engine.callbackStats().recordLoad(0.1);
        }
    }
    const auto after = rt::countersForThisThread();

    CHECK(after.allocations == before.allocations);
    CHECK(after.deallocations == before.deallocations);
    CHECK(after.bytesAllocated == before.bytesAllocated);
    CHECK(rt::violationCount() == violationsBefore);
}

TEST_CASE("Blocks larger than the prepared size are rendered in chunks", "[engine]")
{
    AudioEngine engine;
    engine.prepare(48000.0, 64);
    engine.testTone().setGain(0.5f);
    engine.testTone().setEnabled(true);
    engine.prepare(48000.0, 64); // snap the gain ramp to "on"

    std::vector<float> left(1000, 9.0f), right(1000, 9.0f);
    std::array<float*, 2> outputs{left.data(), right.data()};
    {
        const rt::ScopedAudioThread audioThread;
        engine.process(outputs.data(), 2, 1000);
    }

    CHECK(left == right);
    CHECK(std::none_of(left.begin(), left.end(), [](float s) { return s == 9.0f; }));
    CHECK(std::any_of(left.begin(), left.end(), [](float s) { return s > 0.4f; }));
}

TEST_CASE("Null output channels are skipped and an unprepared engine outputs silence", "[engine]")
{
    AudioEngine engine;
    std::vector<float> left(128, 1.0f);
    std::array<float*, 2> outputs{left.data(), nullptr};
    engine.process(outputs.data(), 2, 128);
    CHECK(std::all_of(left.begin(), left.end(), [](float s) { return s == 0.0f; }));
}
