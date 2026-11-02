#pragma once

#include "core/JobPool.h"
#include "engine/SessionController.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lattice::ui
{

/** The status bar along the bottom: background jobs on the left ("Indexing library 40%, 2 more"),
    the set's file name and whether it has unsaved changes on the right. */
class StatusBar final : public juce::Component
{
public:
    StatusBar(SessionController& session, JobPool& jobs);

    /** Reads the jobs and the set's file state again (the shell calls it when either changes). */
    void refresh();
    juce::String jobsText() const { return jobText; }
    juce::String setText() const { return fileText; }

    int preferredHeight() const;
    void paint(juce::Graphics&) override;

private:
    SessionController& session;
    JobPool& jobs;
    juce::String jobText, fileText;
};

} // namespace lattice::ui
