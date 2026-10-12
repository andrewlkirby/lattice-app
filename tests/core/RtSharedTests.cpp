#include "core/RtShared.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>

using lattice::RtShared;

TEST_CASE("RtShared publishes and retires", "[core][rt]")
{
    RtShared<int> shared(std::make_shared<const int>(1));
    std::weak_ptr<const int> first = shared.published();

    {
        const RtShared<int>::ReadScope scope(shared);
        CHECK(*scope.get() == 1);

        shared.publish(std::make_shared<const int>(2));
        CHECK(*scope.get() == 2); // a new load sees the new object...
        CHECK_FALSE(first.expired()); // ...but the old one is kept while a reader is open
        CHECK(shared.retiredCount() == 1);
    }

    shared.collect();
    CHECK(first.expired());
    CHECK(shared.retiredCount() == 0);
}

TEST_CASE("RtShared frees immediately when no reader is open", "[core][rt]")
{
    RtShared<int> shared(std::make_shared<const int>(1));
    std::weak_ptr<const int> first = shared.published();
    shared.publish(std::make_shared<const int>(2));
    CHECK(first.expired());
    const RtShared<int>::ReadScope scope(shared);
    CHECK(*scope.get() == 2);
}

TEST_CASE("RtShared starts empty", "[core][rt]")
{
    RtShared<int> shared;
    const RtShared<int>::ReadScope scope(shared);
    CHECK(scope.get() == nullptr);
}
