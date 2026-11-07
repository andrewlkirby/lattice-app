#include "ui/clip/ClipBindings.h"

#include "ui/clip/NoteGrid.h"

#include <algorithm>

namespace lattice::ui::clipValues
{

namespace
{
const juce::StringArray launchModes{"Trigger", "Gate", "Toggle"};
const juce::StringArray quantizations{"Global", "None", "8 Bars", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8", "1/16"};
const juce::StringArray followActions{"None", "Stop", "Play Again", "Previous", "Next", "First", "Last", "Any", "Other"};
const juce::StringArray denominators{"1", "2", "4", "8", "16"};

using Matcher = std::function<bool(const juce::ValueTree&, const juce::Identifier&)>;

Matcher matchesClip(const juce::ValueTree& c, std::initializer_list<juce::Identifier> props)
{
    std::vector<juce::Identifier> list(props);
    return [c, list](const juce::ValueTree& tree, const juce::Identifier& p) {
        return tree == c && std::find(list.begin(), list.end(), p) != list.end();
    };
}

/** A time in sixteenths, read in the clip's bars.beats.sixteenths (its signature as it is when
    shown: an encoder's binding outlives a change of it). */
ParameterSpec timeSpec(LiveSet& set, const juce::ValueTree& c, const juce::String& id, const juce::String& name, bool isLength,
                       float minimum)
{
    const auto bars = [&set, c] {
        const auto [num, den] = set.clipTimeSignature(c);
        return BarsBeats{num, den};
    };
    // The range's minimum is a whole number of sixteenths, so the steps fall on them.
    return ParameterSpec::continuous(id, name, minimum, 4096.0f, isLength ? 4.0f : 0.0f, ParamUnit::Beats)
        .withCentre(32.0f)
        .withInterval(0.25f)
        .inGroup("Clip")
        .withFormatter([bars, isLength](float v) { return isLength ? bars().length(v) : bars().position(v); })
        .withParser([bars, isLength](const juce::String& text) -> std::optional<float> {
            const auto q = isLength ? bars().parseLength(text) : bars().parsePosition(text);
            return q ? std::optional<float>(static_cast<float>(*q)) : std::nullopt;
        });
}

std::unique_ptr<ValueBinding> choice(LiveSet& set, juce::UndoManager* undo, const juce::ValueTree& c, const juce::String& id,
                                     const juce::String& name, const juce::StringArray& names, const juce::Identifier& prop, int offset)
{
    return std::make_unique<ValueBinding>(
        ParameterSpec::choice(id, name, names, offset).inGroup("Launch"),
        [c, prop, offset] { return static_cast<double>(static_cast<int>(c[prop]) + offset); },
        [&set, c, prop, offset](double v) { set.setClipProperty(c, prop, juce::roundToInt(v) - offset); }, undo, c, matchesClip(c, {prop}));
}

std::unique_ptr<ValueBinding> make(LiveSet& set, juce::UndoManager* undo, const juce::ValueTree& c, const juce::String& id)
{
    if (id == "timeSigNum")
        return std::make_unique<ValueBinding>(
            ParameterSpec::integer("clipTimeSigNum", "Signature Numerator", 1, 99, 4).inGroup("Clip"),
            [&set, c] { return static_cast<double>(set.clipTimeSignature(c).first); },
            [&set, c](double v) { set.setClipTimeSignature(c, juce::roundToInt(v), set.clipTimeSignature(c).second); }, undo, c,
            matchesClip(c, {ids::clipTimeSigNum, ids::clipTimeSigDen}));
    if (id == "timeSigDen")
        return std::make_unique<ValueBinding>(
            ParameterSpec::choice("clipTimeSigDen", "Signature Denominator", denominators, 2).inGroup("Clip"),
            [&set, c] { return static_cast<double>(denominators.indexOf(juce::String(set.clipTimeSignature(c).second))); },
            [&set, c](double v) {
                set.setClipTimeSignature(c, set.clipTimeSignature(c).first, denominators[juce::jlimit(0, 4, juce::roundToInt(v))].getIntValue());
            },
            undo, c, matchesClip(c, {ids::clipTimeSigNum, ids::clipTimeSigDen}));
    if (id == "launchMode")
        return choice(set, undo, c, "launchMode", "Launch Mode", launchModes, ids::launchMode, 0);
    if (id == "quantization")
        return choice(set, undo, c, "quantization", "Launch Quantization", quantizations, ids::quantization, 1);
    if (id == "legato")
        return ValueBinding::property(ParameterSpec::toggle("legato", "Legato", false).inGroup("Launch"), undo, c, ids::legato,
                                      [&set, c](double v) { set.setClipProperty(c, ids::legato, v > 0.5); });
    if (id == "followA")
        return choice(set, undo, c, "followA", "Follow Action A", followActions, ids::followA, 0);
    if (id == "followB")
        return choice(set, undo, c, "followB", "Follow Action B", followActions, ids::followB, 0);
    if (id == "followChanceA")
        return ValueBinding::property(ParameterSpec::integer("followChanceA", "Follow Chance A", 0, 100, 100, ParamUnit::Percent).inGroup("Launch"),
                                      undo, c, ids::followChanceA,
                                      [&set, c](double v) { set.setClipProperty(c, ids::followChanceA, juce::roundToInt(v)); });
    if (id == "followTime")
        return ValueBinding::property(timeSpec(set, c, "followTime", "Follow Time", true, 0.0f), undo, c, ids::followTime,
                                      [&set, c](double v) { set.setClipProperty(c, ids::followTime, v); });
    if (id == "looping")
        return ValueBinding::property(ParameterSpec::toggle("looping", "Loop", true).inGroup("Loop"), undo, c, ids::looping,
                                      [&set, c](double v) { set.setClipProperty(c, ids::looping, v > 0.5); });
    if (id == "startMarker")
        return ValueBinding::property(timeSpec(set, c, "startMarker", "Start", false, 0.0f), undo, c, ids::startMarker,
                                      [&set, c](double v) { set.setClipProperty(c, ids::startMarker, v); });
    if (id == "loopEnd")
        return std::make_unique<ValueBinding>(
            timeSpec(set, c, "loopEnd", "End", false, 0.25f), [c] { return static_cast<double>(c[ids::loopEnd]); },
            [&set, c](double v) { set.setClipLoop(c, c[ids::loopStart], v); }, undo, c, matchesClip(c, {ids::loopEnd}));
    if (id == "loopStart")
        return std::make_unique<ValueBinding>(
            timeSpec(set, c, "loopStart", "Loop Position", false, 0.0f), [c] { return static_cast<double>(c[ids::loopStart]); },
            [&set, c](double v) {
                // Moves the loop, keeping its length.
                const double length = static_cast<double>(c[ids::loopEnd]) - static_cast<double>(c[ids::loopStart]);
                set.setClipLoop(c, v, v + length);
            },
            undo, c, matchesClip(c, {ids::loopStart, ids::loopEnd}));
    if (id == "loopLength")
        return std::make_unique<ValueBinding>(
            timeSpec(set, c, "loopLength", "Loop Length", true, 0.25f),
            [c] { return static_cast<double>(c[ids::loopEnd]) - static_cast<double>(c[ids::loopStart]); },
            [&set, c](double v) { set.setClipLoop(c, c[ids::loopStart], static_cast<double>(c[ids::loopStart]) + v); }, undo, c,
            matchesClip(c, {ids::loopStart, ids::loopEnd}));
    return nullptr;
}
} // namespace

const juce::StringArray& valueIds()
{
    static const juce::StringArray list{"timeSigNum", "timeSigDen", "launchMode", "quantization", "legato", "followA", "followB",
                                        "followChanceA", "followTime", "looping", "startMarker", "loopEnd", "loopStart", "loopLength"};
    return list;
}

std::unique_ptr<ValueBinding> binding(LiveSet& set, juce::UndoManager* undo, const juce::ValueTree& clip, const juce::String& id)
{
    if (!clip.isValid() || !clip.hasType(ids::clip))
        return nullptr;
    auto b = make(set, undo, clip, id);
    if (b != nullptr)
        b->target = target(clip, id);
    return b;
}

juce::String target(const juce::ValueTree& clip, const juce::String& id)
{
    return "clip:" + juce::String(LiveSet::idOf(clip)) + "/" + id;
}

std::unique_ptr<ValueBinding> bindingFor(LiveSet& set, juce::UndoManager* undo, const juce::String& address)
{
    const auto clip = set.clipById(address.upToFirstOccurrenceOf("/", false, false).getLargeIntValue());
    return binding(set, undo, clip, address.fromFirstOccurrenceOf("/", false, false));
}

} // namespace lattice::ui::clipValues
