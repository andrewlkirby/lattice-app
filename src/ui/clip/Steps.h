#pragma once

#include "ui/clip/NoteEdits.h"
#include "ui/widgets/ValueBinding.h"

#include <memory>
#include <optional>
#include <vector>

namespace lattice::ui::steps
{

// A clip's notes as a step sequencer sees them (docs/06-m5-controllers.md, "Drum layout" and
// "Melodic step sequencer"): a row of steps for each pitch, each step a stepLength long. A note
// belongs to the step nearest its start (within half a step of the step's line), so a note
// nudged off the line, or drawn off it in the piano roll, still shows on its step.

/** One step of one row. */
struct Step
{
    int pitch = 0;
    int index = 0; // from the clip's start: step n starts at n x stepLength

    bool operator==(const Step&) const = default;
};

/** The velocity a step pressed on gets. */
constexpr int defaultVelocity = 100;

/** The step a note starting at `start` shows on. */
int stepOf(double start, double stepLength) noexcept;
/** The notes on a step (indices into `notes`, in order). */
notes::Indices notesIn(const notes::Notes& notes, Step step, double stepLength);
/** The notes on any of the steps. */
notes::Indices notesIn(const notes::Notes& notes, const std::vector<Step>& steps, double stepLength);

/** What a step shows. */
enum class State : std::uint8_t
{
    off,
    on,     // a note starts on it
    muted,  // only muted notes start on it
    tied    // a note from an earlier step still sounds through it
};
/** A row's states for `count` steps from `first`. */
std::vector<State> row(const notes::Notes& notes, int pitch, double stepLength, int first, int count);

/** Pressing a step: its notes go, or (with none) a note starts there, a step long. */
notes::Change toggle(const notes::Notes& notes, Step step, double stepLength, int velocity = defaultVelocity);

/** What a held step's encoder edits. */
enum class Field : std::uint8_t
{
    velocity,
    length,
    nudge // how far from the step's line the notes start (within half a step either way)
};
const char* fieldName(Field field) noexcept;
std::optional<Field> fieldFromName(juce::StringRef name) noexcept;

/** "notes:<clip id>/<step length>/<pitch>:<step>,.../<field>": the notes on some steps, as an
    encoder edits them (ui::bindingForTarget reads it). */
juce::String target(const juce::ValueTree& clip, const std::vector<Step>& held, double stepLength, Field field);

/** The value of the notes on the steps, edited together: setting it sets every one (the first
    note's is what it shows). Each change is undoable, a gesture one step. Null for a clip with
    no notes on them. */
std::unique_ptr<ValueBinding> binding(LiveSet& set, juce::UndoManager* undo, const juce::ValueTree& clip,
                                      std::vector<Step> held, double stepLength, Field field);
/** binding() for an address target() made (the part after "notes:"); null if it isn't one. */
std::unique_ptr<ValueBinding> bindingFor(LiveSet& set, juce::UndoManager* undo, const juce::String& address);

} // namespace lattice::ui::steps
