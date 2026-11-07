#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <optional>
#include <vector>

namespace lattice::hosting::scan
{

/** What the app and LatticePluginScanner say to each other (docs/07-m6-plugins-external.md, 6a).

    The app writes the files to scan to a jobs file, one per line, and starts the scanner on it
    from a given line. The scanner appends what it finds to a results file, a line at a time, each
    flushed before the next step: so whatever the scanner was doing when it crashed or hung, the
    app knows which file it was. A file, not the scanner's output: plugins print to stdout. */

/** One file (or, for formats without files, one identifier) to scan. */
struct Job
{
    juce::String format; // juce::AudioPluginFormat::getName()
    juce::String identifier;

    bool operator==(const Job& o) const { return format == o.format && identifier == o.identifier; }
};

/** A plugin found: its description, and what Lattice keeps beside it. */
struct ScannedPlugin
{
    juce::PluginDescription description;
    // Known only where the format tells it without an instance (CLAP's note ports); otherwise -1
    // until the plugin is first loaded (6b).
    int midiInputs = -1;
    int midiOutputs = -1;
    int mpe = -1; // 1: takes MPE

    /** The description's XML, with Lattice's attributes added. */
    std::unique_ptr<juce::XmlElement> toXml() const;
    static std::optional<ScannedPlugin> fromXml(const juce::XmlElement& xml);
    /** Unique and stable across scans: the description's identifier string. */
    juce::String key() const { return description.createIdentifierString(); }
};

juce::String writeJobs(const std::vector<Job>& jobs);
std::vector<Job> readJobs(const juce::String& text);

/** A line of the results file. */
struct Line
{
    enum class Type
    {
        begin,  // the scanner starts on a job
        plugin, // a plugin the job's file holds
        end,    // the job is done
        fail,   // the format couldn't read the job's file (text: why)
        crash,  // the scanner is crashing on the job (text: how), written as it goes down
        done,   // every job is done
    };
    Type type = Type::begin;
    int job = -1;
    juce::String text;
    std::optional<ScannedPlugin> plugin;
};

juce::String beginLine(int job);
juce::String pluginLine(int job, const ScannedPlugin& plugin);
juce::String endLine(int job);
juce::String failLine(int job, const juce::String& why);
juce::String crashLine(int job, const juce::String& how);
juce::String doneLine();

/** One line, without its newline. Empty for anything else (a damaged line). */
std::optional<Line> parseLine(const juce::String& line);

/** Escapes tabs, newlines and backslashes, so a field fits on its line. */
juce::String escape(const juce::String& text);
juce::String unescape(const juce::String& text);

} // namespace lattice::hosting::scan
