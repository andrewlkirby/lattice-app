#pragma once

#include "core/RealtimeCheck.h"

#include <catch2/catch_test_macros.hpp>

// Tests that count allocations need Lattice's replacement operator new/delete. ThreadSanitizer
// builds can't have them (its runtime defines its own), so those tests skip there; every other
// build, including RTSan, runs them.
#define LATTICE_REQUIRE_ALLOCATION_HOOKS()                                                          \
    do                                                                                              \
    {                                                                                               \
        if (!::lattice::rt::hooksInstalled())                                                       \
            SKIP("Allocation hooks are not linked into this build (ThreadSanitizer)");              \
    } while (false)
