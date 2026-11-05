#pragma once

#include "ui/controllers/SurfaceLearner.h"
#include "ui/controllers/SurfaceManager.h"

#include <juce_gui_basics/juce_gui_basics.h>

namespace lattice::ui
{

/** The learn wizard (Preferences > Controllers > Learn...; docs/06-m5-controllers.md, "Learn
    wizard"): a step at a time, a device's controls become a definition file in the user's
    Surfaces folder, and (if wanted) a surface at once. SurfaceLearner does the work; this shows
    the steps and listens to the device (SurfaceManager::beginLearning). */
class LearnWizard final : public juce::Component
{
public:
    LearnWizard(SurfaceManager& manager, std::function<void()> finished);
    ~LearnWizard() override;

    void paint(juce::Graphics&) override;
    void resized() override;

    /** Tests: the learner, the start button's work, and saving. */
    SurfaceLearner& learner() noexcept { return learn; }
    bool begin(const juce::String& name, const juce::String& input, const juce::String& output, int rows, int columns);
    /** Writes the file (and adds the surface if useNow); the file, or none if it couldn't. */
    juce::File save(bool useNow);

private:
    void refresh();
    void heard(const juce::MidiMessage& m);
    void tryLights(int choice);
    int nextIndex(control::Role role) const;

    SurfaceManager& manager;
    std::function<void()> onFinished;
    SurfaceLearner learn;
    bool started = false;
    SurfaceLearner::Step shownStep = SurfaceLearner::Step::done;
    juce::String input, output;
    std::unique_ptr<control::generic::MidiSender> lights;

    // Setting up.
    juce::TextEditor name;
    juce::ComboBox inputs, outputs, rowsBox, columnsBox;
    juce::TextButton start{"Start"};
    // Each step's.
    juce::TextButton clockwise{"Turned Clockwise"}, encoderDone{"Done With This One"}, noMoreEncoders{"No More Encoders"};
    juce::ComboBox role;
    juce::TextButton assign{"Give It This Role"}, noMoreButtons{"No More Buttons"};
    juce::ComboBox lightChoice;
    juce::TextButton tryIt{"Try"}, useLights{"Use These Lights"};
    juce::ToggleButton useNow{"Use it as a surface now"};
    juce::TextButton saveButton{"Save"}, cancel{"Cancel"};
    juce::String message;
    juce::Rectangle<int> textArea;
};

} // namespace lattice::ui
