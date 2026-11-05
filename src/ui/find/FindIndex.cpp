#include "ui/find/FindIndex.h"

#include "core/model/UndoNames.h"

#include <algorithm>
#include <functional>

namespace lattice::ui
{
namespace
{
// The undo history's location format (undoNames).
const juce::String arrow = undoNames::arrow();
const juce::String dot = undoNames::dot();
} // namespace

int FindIndex::scoreFor(const juce::String& name, const juce::String& typed)
{
    const auto n = name.toLowerCase(), t = typed.toLowerCase().trim();
    if (n == t)
        return 3;
    if (n.startsWith(t))
        return 2;
    return 1;
}

const std::vector<FindIndex::Setting>& FindIndex::settings()
{
    // The Preferences' tabs (PreferencesView::Page order) and what's on them.
    static const std::vector<Setting> list{
        {"Audio Device", "Audio", 0},          {"Sample Rate", "Audio", 0},
        {"Buffer Size", "Audio", 0},           {"Latency", "Audio", 0},
        {"Test Tone", "Audio", 0},             {"Tone Frequency", "Audio", 0},
        {"Tone Level", "Audio", 0},            {"Audio Load", "Audio", 0},
        {"MIDI Inputs", "MIDI", 1},            {"MIDI Track Input", "MIDI", 1},
        {"MIDI Remote Input", "MIDI", 1},      {"MIDI Clock Sync", "MIDI", 1},
        {"Controllers", "Controllers", 2},     {"Push", "Controllers", 2},
        {"Virtual Push Rows", "Controllers", 2}, {"Virtual Push Columns", "Controllers", 2},
        {"Virtual Push Encoders", "Controllers", 2}, {"Pad Velocity", "Controllers", 2},
        {"Velocity Curve", "Controllers", 2},  {"Pad Pressure", "Controllers", 2},
        {"Pressure Threshold", "Controllers", 2}, {"Touch Strip", "Controllers", 2},
        {"Control Surfaces", "Controllers", 2}, {"Surface Monitor", "Controllers", 2},
        {"Launchpad", "Controllers", 2},       {"APC", "Controllers", 2},
        {"Learn a Controller", "Controllers", 2}, {"Ableton Push", "Controllers", 2},
        {"Per-Note Expression", "Controllers", 2}, {"MPE", "Controllers", 2},
        {"Virtual Push Selection", "Controllers", 2},
        {"Theme", "Look & Feel", 3},           {"Zoom", "Look & Feel", 3},
        {"Colours", "Look & Feel", 3},         {"Places", "Library", 4},
        {"Library Folders", "Library", 4},     {"Rescan Library", "Library", 4},
        {"Multi-threaded Audio", "CPU", 5},    {"Worker Threads", "CPU", 5},
        {"New Tracks' Tuning", "Tuning", 6},   {"Default Tuning", "Tuning", 6},
        {"New Tracks' High Notes", "Tuning", 6}, {"Computer Keyboard Keys", "Computer Keyboard", 7},
        {"Note Keys", "Computer Keyboard", 7}, {"Range Step", "Computer Keyboard", 7},
        {"Shortcuts", "Shortcuts", 8},         {"Key Commands", "Shortcuts", 8},
    };
    return list;
}

FindIndex::FindIndex(DeviceContext& c, CommandRegistry& r) : context(c), commands(r) {}

juce::StringArray FindIndex::wordsOf(const juce::String& text)
{
    // Split at spaces and punctuation (the middle dot too: "Synth · 2 MIDI").
    auto words = juce::StringArray::fromTokens(text.toLowerCase().replace(juce::String::fromUTF8("\xc2\xb7"), " "), " ()/-:,&'.", "");
    words.removeEmptyStrings();
    return words;
}

namespace
{
/** Whether `word` starts one of `in`. */
bool starts(const juce::String& word, const juce::StringArray& in)
{
    for (const auto& w : in)
        if (w.startsWith(word))
            return true;
    return false;
}
} // namespace

bool FindIndex::matches(const juce::StringArray& words, const juce::String& haystack)
{
    const auto hayWords = wordsOf(haystack);
    for (const auto& w : words)
        if (!starts(w, hayWords))
            return false;
    return true;
}

const std::vector<FindIndex::ParameterEntry>& FindIndex::parametersOf(const juce::String& typeId) const
{
    auto& entries = byType[typeId];
    if (!entries.empty())
        return entries;
    const auto* spec = context.specs.forType(typeId);
    if (spec == nullptr)
        return entries;
    // Each parameter's group: the device's own grouping (what its panel shows), else its spec's.
    std::vector<juce::String> groupOf(static_cast<std::size_t>(spec->numParameters()));
    for (const auto& g : spec->parameterGroups())
        for (const int i : g.parameters)
            if (i >= 0 && i < spec->numParameters() && groupOf[static_cast<std::size_t>(i)].isEmpty())
                groupOf[static_cast<std::size_t>(i)] = g.name;
    for (int i = 0; i < spec->numParameters(); ++i)
    {
        const auto& p = spec->parameter(i).spec();
        ParameterEntry e;
        e.id = p.id;
        e.name = p.name;
        e.group = groupOf[static_cast<std::size_t>(i)].isNotEmpty() ? groupOf[static_cast<std::size_t>(i)] : p.group;
        e.words = wordsOf(e.name + " " + e.group);
        entries.push_back(std::move(e));
    }
    return entries;
}

std::vector<FindIndex::Result> FindIndex::search(const juce::String& text, int selectedTrack, bool commandsOnly, int limit) const
{
    const auto words = wordsOf(text);
    std::vector<Result> out;
    if (words.isEmpty())
        return out;
    auto& set = context.set();

    if (!commandsOnly)
    {
        // Every device: in each chain, and inside racks' chains.
        std::function<void(const juce::ValueTree&, int)> devicesIn = [&](const juce::ValueTree& node, int section) {
            if (node.hasType(ids::device))
            {
                const auto deviceName = context.nameOf(node);
                const auto deviceWords = wordsOf(deviceName);
                // What it's in: a rack's chain and the rack, then the track (undoNames::locationOf).
                const auto outer = undoNames::locationOf(node.getParent(), context.registry());
                for (const auto& p : parametersOf(node[ids::deviceType].toString()))
                {
                    bool all = true;
                    for (const auto& w : words)
                        all = all && (starts(w, p.words) || starts(w, deviceWords));
                    if (!all)
                        continue;
                    Result r;
                    r.kind = Kind::parameter;
                    r.name = p.name;
                    r.location = deviceName + (p.group.isNotEmpty() ? arrow + p.group : juce::String()) + (outer.isNotEmpty() ? dot + outer : juce::String());
                    r.section = section;
                    r.score = scoreFor(p.name, text);
                    r.device = node;
                    r.parameterId = p.id;
                    out.push_back(std::move(r));
                }
            }
            for (const auto& child : node)
                devicesIn(child, section);
        };
        // Mixer controls of one track, return or the master.
        const auto mixer = [&](const juce::ValueTree& owner, const juce::String& ownerName, int section, bool isTrack, bool isGroup) {
            std::vector<std::pair<juce::String, juce::String>> controls{{"Volume", "volumeDb"}, {"Pan", "pan"}, {"Track On", "mute"}, {"Solo", "solo"}};
            if (owner.hasType(ids::master))
                controls = {{"Volume", "volumeDb"}, {"Pan", "pan"}};
            if (isTrack && !isGroup)
            {
                controls.push_back({"Arm", "arm"});
                controls.push_back({"Tuning", "tuningId"});
                controls.push_back({"High Notes", "highNotePolicy"});
                controls.push_back({"Bend", "bendMode"});
                controls.push_back({"Bend Range", "bendRange"});
                controls.push_back({"MIDI From", "midiInput"});
                controls.push_back({"MIDI Channel", "midiChannel"});
                controls.push_back({"Monitor", "monitoring"});
            }
            if (isTrack)
                for (int r = 0; r < set.numReturns(); ++r)
                    controls.push_back({"Send to " + set.returnTrack(r)[ids::name].toString(), "send:" + juce::String(LiveSet::idOf(set.returnTrack(r)))});
            for (const auto& [name, property] : controls)
            {
                if (!matches(words, name + " Mixer " + ownerName))
                    continue;
                Result r;
                r.kind = Kind::mixer;
                r.name = name;
                r.location = "Mixer" + dot + ownerName;
                r.section = section;
                r.score = scoreFor(name, text);
                r.owner = owner;
                r.parameterId = property;
                out.push_back(std::move(r));
            }
        };
        for (int t = 0; t < set.numTracks(); ++t)
        {
            const auto track = set.track(t);
            const int section = t == selectedTrack ? 0 : 1;
            const auto name = track[ids::name].toString();
            devicesIn(LiveSet::chainOf(track), section);
            mixer(track, name, section, true, set.isGroupTrack(t));
        }
        for (int r = 0; r < set.numReturns(); ++r)
        {
            const auto ret = set.returnTrack(r);
            devicesIn(LiveSet::chainOf(ret), 1);
            mixer(ret, ret[ids::name].toString(), 1, false, false);
        }
        devicesIn(LiveSet::chainOf(set.master()), 1);
        mixer(set.master(), "Master", 1, false, false);
        if (extra)
            for (auto& r : extra(words, text))
                out.push_back(std::move(r));

        for (const auto& s : settings())
            if (matches(words, s.name + " " + s.tab + " Preferences"))
            {
                Result r;
                r.kind = Kind::setting;
                r.name = s.name;
                r.location = "Preferences" + arrow + s.tab;
                r.section = 2;
                r.score = scoreFor(s.name, text);
                r.page = s.page;
                out.push_back(std::move(r));
            }
    }
    for (const auto* c : commands.all())
        if (matches(words, c->name + " " + c->category))
        {
            Result r;
            r.kind = Kind::command;
            r.name = c->name;
            const auto keys = commands.shortcutText(c->id);
            r.location = c->category + (keys.isNotEmpty() ? dot + keys : juce::String());
            r.section = 3;
            r.score = scoreFor(c->name, text);
            r.commandId = c->id;
            out.push_back(std::move(r));
        }

    std::stable_sort(out.begin(), out.end(), [](const Result& a, const Result& b) {
        if (a.section != b.section)
            return a.section < b.section;
        return a.score > b.score;
    });
    if (limit > 0 && static_cast<int>(out.size()) > limit)
        out.resize(static_cast<std::size_t>(limit));
    return out;
}

} // namespace lattice::ui
