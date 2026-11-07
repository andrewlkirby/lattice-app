// Test plugins that go wrong while they're scanned (docs/07-m6-plugins-external.md, "Tests" 1):
// built as a VST3 or a CLAP (LATTICE_BROKEN_VST3 / LATTICE_BROKEN_CLAP), crashing or hanging
// (LATTICE_BROKEN_CRASH / LATTICE_BROKEN_HANG) at the first call a host makes.

#include <chrono>
#include <thread>

#if defined(_WIN32)
#define LATTICE_EXPORT extern "C" __declspec(dllexport)
#else
#define LATTICE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace
{
[[maybe_unused]] void goWrong()
{
#if defined(LATTICE_BROKEN_CRASH)
    volatile int* nowhere = nullptr;
    *nowhere = 1;
#elif defined(LATTICE_BROKEN_HANG)
    for (;;)
        std::this_thread::sleep_for(std::chrono::seconds(1));
#endif
}
} // namespace

#if defined(LATTICE_BROKEN_VST3)
LATTICE_EXPORT void* GetPluginFactory()
{
    goWrong();
    return nullptr;
}
#endif

#if defined(LATTICE_BROKEN_CLAP)
#include <clap/clap.h>

namespace
{
bool entryInit(const char*)
{
    goWrong();
    return false;
}
void entryDeinit() {}
const void* entryFactory(const char*) { return nullptr; }
} // namespace

LATTICE_EXPORT const clap_plugin_entry_t clap_entry{CLAP_VERSION_INIT, entryInit, entryDeinit, entryFactory};
#endif
