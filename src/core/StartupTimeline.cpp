#include "core/StartupTimeline.h"

#include "core/RealtimeCheck.h"
#include "platform/Platform.h"

namespace lattice
{

StartupTimeline::StartupTimeline(std::function<double()> clockToUse) : clock(std::move(clockToUse))
{
    if (!clock)
        clock = [] { return platform::secondsSinceProcessStart(); };
}

double StartupTimeline::mark(const juce::String& stage)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    const double now = clock();
    const std::scoped_lock lock(mutex);
    for (const auto& e : stages)
        if (e.stage == stage)
            return e.seconds;
    stages.push_back({stage, now});
    return now;
}

std::vector<StartupTimeline::Entry> StartupTimeline::entries() const
{
    const std::scoped_lock lock(mutex);
    return stages;
}

std::optional<double> StartupTimeline::timeOf(const juce::String& stage) const
{
    const std::scoped_lock lock(mutex);
    for (const auto& e : stages)
        if (e.stage == stage)
            return e.seconds;
    return std::nullopt;
}

juce::String StartupTimeline::toLogLine(const juce::Time& now) const
{
    juce::String line = now.toISO8601(true);
    for (const auto& e : entries())
        line << " | " << e.stage << "=" << juce::String(e.seconds, 3);
    return line;
}

bool StartupTimeline::appendTo(const juce::File& logFile, int maxLines) const
{
    if (!logFile.getParentDirectory().createDirectory())
        return false;

    juce::StringArray lines;
    if (logFile.existsAsFile())
        lines.addLines(logFile.loadFileAsString());
    lines.removeEmptyStrings();
    lines.add(toLogLine());
    if (lines.size() > maxLines)
        lines.removeRange(0, lines.size() - maxLines);

    return logFile.replaceWithText(lines.joinIntoString("\n") + "\n");
}

} // namespace lattice
