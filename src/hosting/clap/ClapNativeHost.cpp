// The native window a CLAP plugin's GUI goes in (ClapEditor), one per OS. Kept apart so the OS
// headers stay out of everything else.

#include "hosting/clap/ClapEditor.h"

#if JUCE_WINDOWS
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace lattice::hosting
{

#if JUCE_WINDOWS
std::unique_ptr<juce::Component> makeNativeHost(void*& handle)
{
    // A plain window: the plugin makes its own inside it. HWNDComponent makes it a child of the
    // editor's window and keeps it over the editor (and destroys it).
    HWND window = CreateWindowExW(0, L"STATIC", L"", WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS, 0, 0, 1, 1, nullptr,
                                  nullptr, GetModuleHandleW(nullptr), nullptr);
    handle = window;
    if (window == nullptr)
        return nullptr;
    auto component = std::make_unique<juce::HWNDComponent>();
    component->setHWND(window);
    return component;
}
#elif JUCE_LINUX || JUCE_BSD
std::unique_ptr<juce::Component> makeNativeHost(void*& handle)
{
    auto component = std::make_unique<juce::XEmbedComponent>(true, false);
    handle = reinterpret_cast<void*>(static_cast<std::uintptr_t>(component->getHostWindowID()));
    return component;
}
#else
std::unique_ptr<juce::Component> makeNativeHost(void*& handle)
{
    handle = nullptr;
    return nullptr; // macOS: with the macOS port
}
#endif

} // namespace lattice::hosting
