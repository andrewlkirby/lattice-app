#pragma once

#include <juce_core/juce_core.h>

namespace lattice
{

/** The recovery copy autosave keeps (docs/05-m4-ui.md, 4g): `Recovery.lset`, a copy of the set
    with every path in full, and `Recovery.info`, the file the set was saved as (if any) and when
    the copy was made. A normal quit, and any save, clears it; one still there at startup means
    Lattice didn't close properly. Any thread: autosave writes it on the job pool. */
class RecoveryCopy
{
public:
    explicit RecoveryCopy(juce::File folder);

    /** Replaces the copy (written beside it first, then moved, so a crash while writing leaves
        the last good one). */
    bool write(const juce::XmlElement& setXml, const juce::File& original) const;
    bool exists() const;
    void clear() const;

    juce::File setFile() const { return dir.getChildFile("Recovery.lset"); }
    /** The file the set was saved as (none for a set never saved). */
    juce::File original() const;
    juce::Time time() const;

private:
    juce::File infoFile() const { return dir.getChildFile("Recovery.info"); }
    juce::File dir;
};

} // namespace lattice
