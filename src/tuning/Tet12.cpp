#include "tuning/Tet12.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <cmath>

namespace lattice
{

TuningPtr Tet12::make(double referenceHz)
{
    auto table = std::make_shared<TuningTable>();
    table->id = builtinId;
    table->name = "12-TET A" + juce::String(referenceHz, referenceHz == std::floor(referenceHz) ? 0 : 2);
    table->sourceDescription = "Built in";
    table->periodRatio = 2.0;
    table->keyboardPageSize = 12;
    table->usesNoteNames = true;

    for (int n = 0; n < TuningTable::numNotes; ++n)
    {
        table->hz[static_cast<std::size_t>(n)] = referenceHz * std::pow(2.0, (n - 69) / 12.0);
        table->labels[static_cast<std::size_t>(n)] = juce::MidiMessage::getMidiNoteName(n, true, true, 4);
    }
    return table;
}

} // namespace lattice
