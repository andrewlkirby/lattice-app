#include "platform/Platform.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <objbase.h>
#include <avrt.h>

namespace lattice::platform
{

Paths defaultPaths()
{
    // %APPDATA% (roaming) for both user data and settings.
    const auto appData =
        juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory).getChildFile("Lattice");
    return {appData, appData};
}

double secondsSinceProcessStart()
{
    FILETIME creation{}, exitTime{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exitTime, &kernel, &user))
        return 0.0;

    FILETIME now{};
    GetSystemTimePreciseAsFileTime(&now);

    const auto toTicks = [](const FILETIME& ft) {
        return (static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    };
    const auto start = toTicks(creation);
    const auto current = toTicks(now);
    return current > start ? static_cast<double>(current - start) * 1.0e-7 : 0.0; // 100 ns ticks
}

juce::StringArray preferredAudioDeviceTypes()
{
    // WASAPI exclusive is the default on the reference laptop (docs/perf/reference-machine.md).
    // If ASIO (e.g. ASIO4ALL) wins the M0 comparison, it moves to the front of this list.
    return {"Windows Audio (Exclusive Mode)", "Windows Audio (Low Latency Mode)", "Windows Audio"};
}

bool joinProAudioClass()
{
    DWORD taskIndex = 0;
    return AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex) != nullptr;
}

ScopedAudioDeviceThread::ScopedAudioDeviceThread()
{
    // S_FALSE means COM was already initialised on this thread; it still needs a matching release.
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    needsRelease = SUCCEEDED(hr);
}

ScopedAudioDeviceThread::~ScopedAudioDeviceThread()
{
    if (needsRelease)
        CoUninitialize();
}

bool audioDeviceTypeNeedsMessageThread(const juce::String& typeName)
{
    return typeName == "ASIO";
}

} // namespace lattice::platform
