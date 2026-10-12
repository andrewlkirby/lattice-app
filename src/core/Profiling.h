#pragma once

// Tracy profiler markers. They compile to nothing unless the build is configured with
// -DLATTICE_PROFILE=ON (docs/09-performance-charter.md §2). Tracy zones do not allocate, so they
// are safe on the audio thread.

#if LATTICE_PROFILE
#include <tracy/Tracy.hpp>
#define LATTICE_ZONE(name) ZoneScopedN(name)
#define LATTICE_FRAME_MARK_NAMED(name) FrameMarkNamed(name)
#define LATTICE_THREAD_NAME(name) tracy::SetThreadName(name)
#else
#define LATTICE_ZONE(name)
#define LATTICE_FRAME_MARK_NAMED(name)
#define LATTICE_THREAD_NAME(name)
#endif
