#include "platform/Platform.h"

#include <catch2/catch_session.hpp>
#include <juce_events/juce_events.h>

#if JUCE_WINDOWS
#include <windows.h>

#include <cstdio>
#include <cstring>

namespace
{
/** A crash on any thread prints that thread's stack as module offsets before Catch reports it (Catch
    can only say where the main thread was, which may be another test's code). Offsets resolve with
    the linker's map file. */
LONG CALLBACK reportCrash(EXCEPTION_POINTERS* info)
{
    const auto code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    void* frames[48] = {};
    const auto count = CaptureStackBackTrace(0, 48, frames, nullptr);
    std::fprintf(stderr, "\n*** Crash 0x%08lx on thread %lu at %p\n", static_cast<unsigned long>(code), GetCurrentThreadId(),
                 info->ExceptionRecord->ExceptionAddress);
    for (USHORT i = 0; i < count; ++i)
    {
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &module))
            GetModuleFileNameA(module, name, MAX_PATH);
        const auto offset = static_cast<unsigned long long>(reinterpret_cast<const char*>(frames[i]) - reinterpret_cast<const char*>(module));
        std::fprintf(stderr, "  %2u  %s+0x%llx\n", static_cast<unsigned>(i), std::strrchr(name, '\\') != nullptr ? std::strrchr(name, '\\') + 1 : name,
                     offset);
    }
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace
#endif

#if JUCE_LINUX
#include <dlfcn.h>

namespace
{
/** JUCE looks up the window manager's atoms (WM_PROTOCOLS...) only if they exist, then sets them
    on every window it opens; on a bare Xvfb display with no window manager they don't exist, and
    the X error that follows ends the process. Creates them first (on a desktop they always exist).
    The connection stays open: a server forgets its atoms when its last client goes. */
void createWindowManagerAtoms()
{
    auto* x11 = dlopen("libX11.so.6", RTLD_LAZY | RTLD_LOCAL);
    if (x11 == nullptr)
        return;
    using OpenDisplay = void* (*) (const char*);
    using InternAtom = unsigned long (*)(void*, const char*, int);
    const auto open = reinterpret_cast<OpenDisplay>(dlsym(x11, "XOpenDisplay"));
    const auto intern = reinterpret_cast<InternAtom>(dlsym(x11, "XInternAtom"));
    if (open == nullptr || intern == nullptr)
        return;
    static void* const display = open(nullptr);
    if (display == nullptr)
        return;
    // Those XWindowSystemUtilities::Atoms takes only if they exist (some are used unchecked).
    for (const auto* name : {"WM_PROTOCOLS", "WM_TAKE_FOCUS", "WM_DELETE_WINDOW", "_NET_WM_PING", "WM_CHANGE_STATE", "WM_STATE",
                             "_NET_WM_WINDOW_TYPE", "_NET_WM_STATE", "_NET_WM_STATE_HIDDEN", "_NET_WM_STATE_MAXIMIZED_HORZ",
                             "_NET_WM_STATE_MAXIMIZED_VERT"})
        intern(display, name, 0); // 0: create it if it doesn't exist
}
} // namespace
#endif

int main(int argc, char* argv[])
{
#if JUCE_WINDOWS
    AddVectoredExceptionHandler(1, reportCrash);
#endif
#if JUCE_LINUX
    createWindowManagerAtoms(); // before JUCE opens the display and reads them
#endif
    // Some JUCE classes (e.g. AudioDeviceManager) expect a MessageManager to exist.
    const juce::ScopedJuceInitialiser_GUI juceInit;
    // The window's library (presets, the browser's index) lives in a folder of the tests' own: one
    // for each test process, so that tests run in parallel (ctest -j) never share files. A name
    // that's only free isn't enough, since processes started together would all pick the same one.
    const auto userData = juce::File::getSpecialLocation(juce::File::tempDirectory)
                              .getChildFile("lattice-tests-userdata-" + juce::Uuid().toString());
    userData.createDirectory();
    lattice::platform::setUserDataFolderForTests(userData);
    const int result = Catch::Session().run(argc, argv);
    userData.deleteRecursively();
    return result;
}
