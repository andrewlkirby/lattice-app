#include "platform/Platform.h"

#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>

namespace lattice::platform
{

Paths defaultPaths()
{
    const auto support = juce::File::getSpecialLocation(juce::File::userApplicationDataDirectory)
                             .getChildFile("Application Support")
                             .getChildFile("Lattice");
    return {support, support};
}

double secondsSinceProcessStart()
{
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    kinfo_proc info{};
    size_t size = sizeof(info);
    if (sysctl(mib, 4, &info, &size, nullptr, 0) != 0)
        return 0.0;

    timeval now{};
    gettimeofday(&now, nullptr);
    const auto& start = info.kp_proc.p_starttime;
    const double elapsed = static_cast<double>(now.tv_sec - start.tv_sec)
                         + static_cast<double>(now.tv_usec - start.tv_usec) * 1.0e-6;
    return elapsed > 0.0 ? elapsed : 0.0;
}

juce::StringArray preferredAudioDeviceTypes()
{
    return {"CoreAudio"};
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
