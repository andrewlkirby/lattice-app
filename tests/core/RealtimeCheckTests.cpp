#include "core/RealtimeCheck.h"
#include "support/Escape.h"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <thread>

using namespace lattice;

namespace
{
int handlerCalls = 0;

void countingHandler(const char*, std::size_t)
{
    ++handlerCalls;
}

struct HandlerScope
{
    HandlerScope() : previous(rt::setViolationHandler(&countingHandler)) { handlerCalls = 0; }
    ~HandlerScope() { rt::setViolationHandler(previous); }
    rt::ViolationHandler previous;
};
} // namespace

TEST_CASE("Allocation hooks are linked into the tests", "[core][rt]")
{
    CHECK(rt::hooksInstalled());

    // Explicit operator new/delete calls: unlike new-expressions, the compiler may not remove them.
    const auto before = rt::countersForThisThread();
    void* p = ::operator new(sizeof(int));
    test::escape(p);
    ::operator delete(p);
    const auto after = rt::countersForThisThread();

    CHECK(after.allocations == before.allocations + 1);
    CHECK(after.deallocations == before.deallocations + 1);
    CHECK(after.bytesAllocated >= before.bytesAllocated + sizeof(int));
}

TEST_CASE("ScopedAudioThread marks only the current thread, and nests", "[core][rt]")
{
    CHECK_FALSE(rt::isAudioThread());
    {
        const rt::ScopedAudioThread outer;
        CHECK(rt::isAudioThread());
        {
            const rt::ScopedAudioThread inner;
            CHECK(rt::isAudioThread());
        }
        CHECK(rt::isAudioThread());

        bool otherThreadIsAudio = true;
        std::thread([&] { otherThreadIsAudio = rt::isAudioThread(); }).join();
        CHECK_FALSE(otherThreadIsAudio);
    }
    CHECK_FALSE(rt::isAudioThread());
}

TEST_CASE("Allocating on the audio thread is reported", "[core][rt]")
{
    const HandlerScope handler;
    const auto violationsBefore = rt::violationCount();

    {
        const rt::ScopedAudioThread audioThread;
        void* p = ::operator new(sizeof(int)); // deliberate violation
        test::escape(p);
        ::operator delete(p); // and another one
    }

    CHECK(rt::violationCount() == violationsBefore + 2);
    CHECK(handlerCalls == 2);
}

TEST_CASE("Allocating off the audio thread is not reported", "[core][rt]")
{
    const HandlerScope handler;
    const auto violationsBefore = rt::violationCount();
    void* p = ::operator new(100);
    test::escape(p);
    ::operator delete(p);
    CHECK(rt::violationCount() == violationsBefore);
    CHECK(handlerCalls == 0);
}

TEST_CASE("RT_ASSERT_NOT_AUDIO_THREAD reports only on the audio thread", "[core][rt]")
{
    const HandlerScope handler;
    RT_ASSERT_NOT_AUDIO_THREAD();
    CHECK(handlerCalls == 0);
    {
        const rt::ScopedAudioThread audioThread;
        RT_ASSERT_NOT_AUDIO_THREAD();
    }
    CHECK(handlerCalls == 1);
}

namespace
{
int allocatingHandlerCalls = 0;

void allocatingHandler(const char*, std::size_t)
{
    ++allocatingHandlerCalls;
    auto inner = std::make_unique<int>(1); // must not be reported again
    test::escape(inner.get());
}
} // namespace

TEST_CASE("The violation handler may allocate without recursing", "[core][rt]")
{
    allocatingHandlerCalls = 0;
    const auto previous = rt::setViolationHandler(&allocatingHandler);
    {
        const rt::ScopedAudioThread audioThread;
        RT_ASSERT_NOT_AUDIO_THREAD();
        CHECK(rt::isAudioThread()); // restored after the handler
    }
    rt::setViolationHandler(previous);
    CHECK(allocatingHandlerCalls == 1);
}
