#pragma once

#include "tuning/TuningTable.h"

#include <optional>
#include <vector>

namespace lattice
{

/** Scala scale (.scl) and keyboard mapping (.kbm) files, as specified at
    https://www.huygens-fokker.org/scala/scl_format.html and help.htm#mappings.

    Without a .kbm, the default mapping is used: linear, MIDI note 60 is scale degree 0 and sounds
    at 261.6256 Hz (middle C), every note 0..127 is mapped. */
struct ScalaParser
{
    struct Scale
    {
        juce::String description;
        std::vector<double> ratios; // degrees 1..N as frequency ratios; the last one is the period
    };

    struct KeyboardMapping
    {
        int mapSize = 0;            // 0 = linear
        int firstNote = 0;
        int lastNote = 127;
        int middleNote = 60;        // the note where scale degree 0 is mapped
        int referenceNote = 60;
        double referenceHz = 261.6255653005986;
        int formalOctaveDegree = 0; // 0 = the scale's own size
        std::vector<int> map;       // -1 = unmapped ('x')
    };

    struct ScaleResult
    {
        std::optional<Scale> scale;
        juce::String error;
        int line = 0;
    };

    struct MappingResult
    {
        std::optional<KeyboardMapping> mapping;
        juce::String error;
        int line = 0;
    };

    static ScaleResult parseScale(const juce::String& text);
    static MappingResult parseMapping(const juce::String& text);

    /** Builds the 128-note table. */
    static TuningLoadResult makeTable(const Scale& scale, const KeyboardMapping& mapping, juce::String id,
                                      juce::String name, juce::String sourceDescription);

    /** Parse .scl text (and optional .kbm text) straight to a table. */
    static TuningLoadResult parse(const juce::String& sclText, const juce::String& kbmText, juce::String id,
                                  juce::String name, juce::String sourceDescription);
};

} // namespace lattice
