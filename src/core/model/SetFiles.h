#pragma once

#include <juce_data_structures/juce_data_structures.h>

#include <vector>

namespace lattice
{
class LiveSet;

/** The files a set uses, and keeping them with it (docs/05-m4-ui.md, 4g): samples are named by the
    `file` property of a device's STATE (a Sampler's sample, a Drum Rack pad's). */
namespace setfiles
{

/** One file the set uses: the STATE node naming it. */
struct Reference
{
    juce::ValueTree state;
    juce::File file;
};

/** Every file the set (or any tree) names, in document order. */
std::vector<Reference> references(const juce::ValueTree& root);
/** Those whose file isn't there. */
std::vector<Reference> missing(const juce::ValueTree& root);

/** Saving: paths of files inside `folder` (the set's) are written relative to it, with '/', so the
    set and its folder can move together; other paths stay absolute. */
void makeRelative(juce::XmlElement& setXml, const juce::File& folder);
/** Loading: relative paths are resolved against `folder`. */
void makeAbsolute(juce::XmlElement& setXml, const juce::File& folder);

/** Collect All: copies every file the set uses that isn't already in `folder` into
    `folder`/Samples (a name already taken by another file gets a number), and points the set at
    the copies, through the set's undo manager. Missing files are reported, not copied. */
struct CollectResult
{
    int copied = 0;
    juce::StringArray failed; // "path: reason"
};
CollectResult collect(LiveSet& set, const juce::File& folder);

/** Points every reference to `from` at `to` (one undo step's worth of changes, in the set's
    undo manager): relinking a moved file. Returns how many were changed. */
int relink(LiveSet& set, const juce::File& from, const juce::File& to);

} // namespace setfiles
} // namespace lattice
