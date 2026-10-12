#pragma once

#include "tuning/TuningTable.h"

namespace lattice
{

/** Twelve-tone equal temperament, the default tuning. A4 (MIDI note 69) = referenceHz;
    labels are note names with middle C (note 60) as C4. */
struct Tet12
{
    static constexpr const char* builtinId = "builtin.12tet";

    static TuningPtr make(double referenceHz = 440.0);
};

} // namespace lattice
