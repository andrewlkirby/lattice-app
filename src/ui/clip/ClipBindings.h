#pragma once

#include "ui/widgets/ValueBinding.h"

#include <memory>

namespace lattice::ui::clipValues
{

// A clip's values as the Clip view's panel and Clip mode's encoders (docs/06-m5-controllers.md,
// "Clip mode") both edit them, so a value turned on a controller is the same undo step a drag on
// the panel makes. Each has an ID, and an address "clip:<clip id>/<value id>" (MIDI Map can map it).
//
//   Clip:   timeSigNum, timeSigDen
//   Launch: launchMode, quantization, legato, followA, followB, followChanceA, followTime
//   Loop:   looping, startMarker, loopStart (the loop's position: moving it keeps its length),
//           loopLength, loopEnd

/** Every value's ID, in the panel's order. */
const juce::StringArray& valueIds();

/** A clip's value by ID; null for an ID that isn't one. */
std::unique_ptr<ValueBinding> binding(LiveSet& set, juce::UndoManager* undo, const juce::ValueTree& clip, const juce::String& id);

/** "clip:<clip id>/<value id>" */
juce::String target(const juce::ValueTree& clip, const juce::String& id);
/** binding() for the part of an address after "clip:"; null if it names nothing. */
std::unique_ptr<ValueBinding> bindingFor(LiveSet& set, juce::UndoManager* undo, const juce::String& address);

} // namespace lattice::ui::clipValues
