#pragma once

#include "engine/SessionController.h"
#include "ui/widgets/ValueBinding.h"

#include <memory>
#include <vector>

namespace lattice::ui::clipActions
{

// What's done to a whole clip from a controller (docs/06-m5-controllers.md, "Clip mode") and the
// piano roll alike: each one undo step, named for the clip.

/** The loop twice as long (its notes stay where they are). */
void doubleLoop(SessionController& session, const juce::ValueTree& clip);
/** The loop half as long (at least LiveSet::minLoopLength). */
void halveLoop(SessionController& session, const juce::ValueTree& clip);
/** The loop's notes copied after it and the loop doubled: Push's Double Loop. */
void duplicateLoop(SessionController& session, const juce::ValueTree& clip);
/** Every note's start moved towards the grid by amount (0..1). */
void quantize(SessionController& session, const juce::ValueTree& clip, double gridStep, double amount);
/** quantize() with the set's Quantize settings (quantizeGridOf, quantizeAmountOf). */
void quantize(SessionController& session, const juce::ValueTree& clip);

/** The grids the step sequencers and Quantize offer: (name, length in quarter notes), longest first. */
const std::vector<std::pair<juce::String, double>>& grids();
constexpr int defaultGrid = 3; // 1/16

/** The set's Quantize settings (kept in the set, not undoable): the grid (an index into grids())
    and the amount, 0..100 %. */
int quantizeGridOf(const LiveSet& set);
int quantizeAmountOf(const LiveSet& set);

/** "set/<property>": quantizeGrid, quantizeAmount, fixedRecordBars (the length new recordings
    get: off, or 1 to 16 bars). Null for another property. */
std::unique_ptr<ValueBinding> setBinding(LiveSet& set, juce::UndoManager* undo, const juce::String& property);

/** The lengths Fixed Length offers, in bars (0: off). */
const std::vector<int>& fixedLengths();

} // namespace lattice::ui::clipActions
