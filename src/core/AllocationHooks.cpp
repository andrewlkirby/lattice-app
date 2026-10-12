// Replacement global operator new/delete. They count allocations per thread and report any made on
// a thread marked by ScopedAudioThread. Linked into the tests always, and into the app according to
// LATTICE_RT_CHECKS (see CMakeLists.txt). Every replaceable form is defined so that no allocation
// slips past the hooks and every block is freed by the matching allocator.

#include "core/RealtimeCheck.h"

#include <cstdlib>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace
{
using lattice::rt::detail::noteAllocation;
using lattice::rt::detail::noteDeallocation;

[[maybe_unused]] const bool hooksRegistered = [] {
    lattice::rt::detail::markHooksInstalled();
    return true;
}();

void* allocate(std::size_t size) noexcept
{
    noteAllocation(size);
    return std::malloc(size == 0 ? 1 : size);
}

void* allocateAligned(std::size_t size, std::align_val_t alignment) noexcept
{
    noteAllocation(size);
    const auto align = static_cast<std::size_t>(alignment);
    if (size == 0)
        size = 1;
#if defined(_WIN32)
    return _aligned_malloc(size, align);
#else
    void* p = nullptr;
    return posix_memalign(&p, align < sizeof(void*) ? sizeof(void*) : align, size) == 0 ? p : nullptr;
#endif
}

void release(void* p) noexcept
{
    if (p == nullptr)
        return;
    noteDeallocation();
    std::free(p);
}

void releaseAligned(void* p) noexcept
{
    if (p == nullptr)
        return;
    noteDeallocation();
#if defined(_WIN32)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

void* allocateOrThrow(std::size_t size)
{
    if (void* p = allocate(size))
        return p;
    throw std::bad_alloc();
}

void* allocateAlignedOrThrow(std::size_t size, std::align_val_t alignment)
{
    if (void* p = allocateAligned(size, alignment))
        return p;
    throw std::bad_alloc();
}
} // namespace

// clang-format off
void* operator new  (std::size_t n)                                         { return allocateOrThrow(n); }
void* operator new[](std::size_t n)                                         { return allocateOrThrow(n); }
void* operator new  (std::size_t n, const std::nothrow_t&) noexcept         { return allocate(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept         { return allocate(n); }
void* operator new  (std::size_t n, std::align_val_t a)                     { return allocateAlignedOrThrow(n, a); }
void* operator new[](std::size_t n, std::align_val_t a)                     { return allocateAlignedOrThrow(n, a); }
void* operator new  (std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return allocateAligned(n, a); }
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept { return allocateAligned(n, a); }

void operator delete  (void* p) noexcept                                    { release(p); }
void operator delete[](void* p) noexcept                                    { release(p); }
void operator delete  (void* p, std::size_t) noexcept                       { release(p); }
void operator delete[](void* p, std::size_t) noexcept                       { release(p); }
void operator delete  (void* p, const std::nothrow_t&) noexcept             { release(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept             { release(p); }
void operator delete  (void* p, std::align_val_t) noexcept                  { releaseAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept                  { releaseAligned(p); }
void operator delete  (void* p, std::size_t, std::align_val_t) noexcept     { releaseAligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept     { releaseAligned(p); }
void operator delete  (void* p, std::align_val_t, const std::nothrow_t&) noexcept { releaseAligned(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { releaseAligned(p); }
// clang-format on
