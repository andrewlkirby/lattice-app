#include "ui/shell/StatusBar.h"

#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui
{

StatusBar::StatusBar(SessionController& s, JobPool& j) : session(s), jobs(j)
{
    setTitle("Status");
    refresh();
}

void StatusBar::refresh()
{
    juce::String newJobs;
    const auto list = jobs.jobs();
    if (!list.empty())
    {
        // The first running job, with its progress; the rest as a count.
        auto shown = list.front();
        for (const auto& j : list)
            if (j.running)
            {
                shown = j;
                break;
            }
        newJobs = shown.name;
        if (shown.progress >= 0.0f)
            newJobs << " " << juce::roundToInt(shown.progress * 100.0f) << "%";
        if (list.size() > 1)
            newJobs << ", " << static_cast<int>(list.size() - 1) << " more";
    }

    const auto& file = session.currentFile();
    juce::String newFile = file == juce::File() ? juce::String("Untitled set") : file.getFileNameWithoutExtension();
    if (session.hasUnsavedChanges())
        newFile << " (unsaved changes)";

    if (newJobs != jobText || newFile != fileText)
    {
        jobText = newJobs;
        fileText = newFile;
        repaint();
    }
}

int StatusBar::preferredHeight() const
{
    const auto& t = themeOf(*this);
    return juce::roundToInt(t.fontSmall + t.padding * 1.5f);
}

void StatusBar::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    g.fillAll(t.surface);
    g.setColour(t.border);
    g.fillRect(getLocalBounds().removeFromTop(1));
    auto area = getLocalBounds().reduced(juce::roundToInt(t.padding), 0);
    g.setFont(t.font(t.fontSmall));
    g.setColour(t.textDim);
    g.drawText(fileText, area, juce::Justification::centredRight, true);
    g.setColour(t.text);
    g.drawText(jobText, area, juce::Justification::centredLeft, true);
}

} // namespace lattice::ui
