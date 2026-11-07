#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace lattice::hosting
{

/** The plugin formats Lattice hosts on this OS (docs/07-m6-plugins-external.md, 6a), in the order
    they're preferred when a plugin comes in several: CLAP, then VST3, then the OS's own (AU on
    macOS, LV2 on Linux). */
void addHostedFormats(juce::AudioPluginFormatManager& formats);

/** Their names, in that order. */
juce::StringArray hostedFormatNames();

} // namespace lattice::hosting
