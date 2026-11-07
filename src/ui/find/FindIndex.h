#pragma once

#include "ui/commands/CommandRegistry.h"
#include "ui/device/DeviceContext.h"

#include <functional>
#include <map>
#include <vector>

namespace lattice::ui
{

/** What Find (Ctrl+F) searches (docs/05-m4-ui.md, 4g): every device parameter in the set (on
    tracks, returns, the master and inside rack chains, whether or not its panel is open), every
    mixer control, the Preferences' settings and every command.

    Each result says where it lives ("Synth › Amp Env › Release · 2 MIDI"). Matching is on word
    starts: every word typed starts a word of the result's name, group or device ("amp rel").
    Ranking: the selected track's first, then other tracks', returns' and the master's, then
    settings, then commands; within each, exact names, then names starting with the text, then the
    rest. Made fresh for each search from the document (thousands of entries in well under a
    frame). Message thread only. */
class FindIndex
{
public:
    enum class Kind
    {
        parameter, // a device's
        mixer,     // a track's, return's or the master's volume, pan, sends, switches
        clip,      // a setting of the selected clip (the Clip view's)
        setting,   // a Preferences entry
        command
    };

    struct Result
    {
        Kind kind = Kind::parameter;
        juce::String name;     // "Release"
        juce::String location; // "Synth › Amp Env · 2 MIDI", "Preferences › CPU", "Edit · Ctrl+D"
        int section = 0;       // 0 the selected track, 1 other owners, 2 settings, 3 commands
        int score = 0;         // within a section: higher first
        // What it points at:
        juce::ValueTree device;    // a parameter's device
        juce::String parameterId;  // or the mixer property ("volumeDb", "send:<return id>")
        juce::ValueTree owner;     // a mixer control's track, return or master
        int page = -1;             // a setting's Preferences tab
        juce::String commandId;
        juce::Component::SafePointer<juce::Component> control; // a clip setting's widget
    };

    /** More results (the selected clip's settings, from the controls on screen), for the words
        typed; they come with the selected track's. */
    std::function<std::vector<Result>(const juce::StringArray& words, const juce::String& typed)> extra;

    struct Setting
    {
        juce::String name, tab;
        int page = 0;
    };
    /** The Preferences' entries Find knows, with their tab. */
    static const std::vector<Setting>& settings();

    FindIndex(DeviceContext& context, CommandRegistry& commands);

    /** Results for typed text, best first (at most `limit`; 0: all). commandsOnly: the command
        palette (Ctrl+Shift+P). selectedTrack: whose results come first (-1: none). */
    std::vector<Result> search(const juce::String& text, int selectedTrack, bool commandsOnly = false, int limit = 0) const;

    /** Whether every word starts a word of `haystack` (case-insensitive). */
    static bool matches(const juce::StringArray& words, const juce::String& haystack);
    /** Text as Find's words: lower case, split at spaces and punctuation ("Dry/Wet": dry, wet). */
    static juce::StringArray wordsOf(const juce::String& text);
    /** Exact name 3, a name starting with the text 2, otherwise 1. */
    static int scoreFor(const juce::String& name, const juce::String& typed);

private:
    /** A device type's parameters as Find sees them, made once per type (specs never change). */
    struct ParameterEntry
    {
        juce::String id, name, group;
        juce::StringArray words; // of the name and the group
    };
    /** A device's parameters, as its type describes them (a plugin: as its pages do, asked each time). */
    const std::vector<ParameterEntry>& parametersOf(const juce::ValueTree& device) const;

    DeviceContext& context;
    CommandRegistry& commands;
    mutable std::map<juce::String, std::vector<ParameterEntry>> byType;
    mutable std::map<std::int64_t, std::vector<ParameterEntry>> byDevice;
};

} // namespace lattice::ui
