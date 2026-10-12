#include "platform/Platform.h"

#include <cstdlib>
#include <ctime>
#include <fstream>
#include <sstream>
#include <unistd.h>

namespace lattice::platform
{

Paths defaultPaths()
{
    const auto env = [](const char* name) -> std::optional<std::string> {
        if (const char* v = std::getenv(name))
            return std::string(v);
        return std::nullopt;
    };
    const char* home = std::getenv("HOME");
    const auto dirs = detail::resolveXdgDirs(
        env, home != nullptr ? std::string(home)
                             : juce::File::getSpecialLocation(juce::File::userHomeDirectory)
                                   .getFullPathName()
                                   .toStdString());
    return {juce::File(dirs.userData), juce::File(dirs.settings)};
}

double secondsSinceProcessStart()
{
    // Field 22 of /proc/self/stat is the start time in clock ticks after boot. The command name
    // (field 2) may contain spaces, so parse from the closing parenthesis.
    std::ifstream stat("/proc/self/stat");
    std::string content((std::istreambuf_iterator<char>(stat)), std::istreambuf_iterator<char>());
    const auto close = content.rfind(')');
    if (close == std::string::npos)
        return 0.0;

    std::istringstream fields(content.substr(close + 2));
    std::string field;
    unsigned long long startTicks = 0;
    for (int index = 3; fields >> field; ++index)
    {
        if (index == 22)
        {
            startTicks = std::strtoull(field.c_str(), nullptr, 10);
            break;
        }
    }

    timespec now{};
    clock_gettime(CLOCK_BOOTTIME, &now);
    const double sinceBoot = static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) * 1.0e-9;
    const double started = static_cast<double>(startTicks) / static_cast<double>(sysconf(_SC_CLK_TCK));
    return sinceBoot > started ? sinceBoot - started : 0.0;
}

juce::StringArray preferredAudioDeviceTypes()
{
    // JACK when a server is running (PipeWire provides one on most current desktops), else ALSA.
    return {"JACK", "ALSA"};
}

ScopedAudioDeviceThread::ScopedAudioDeviceThread() = default;
ScopedAudioDeviceThread::~ScopedAudioDeviceThread() = default;

bool joinProAudioClass()
{
    return true; // juce::Thread::startRealtimeThread already asks for real-time scheduling
}

bool audioDeviceTypeNeedsMessageThread(const juce::String&)
{
    return false;
}

} // namespace lattice::platform
