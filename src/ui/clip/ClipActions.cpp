#include "ui/clip/ClipActions.h"

#include "ui/clip/NoteEdits.h"

namespace lattice::ui::clipActions
{

void doubleLoop(SessionController& session, const juce::ValueTree& clip)
{
    if (!clip.isValid())
        return;
    const double start = clip[ids::loopStart], end = clip[ids::loopEnd];
    session.beginEdit("Double loop", clip);
    session.set().setClipLoop(clip, start, start + (end - start) * 2.0);
}

void halveLoop(SessionController& session, const juce::ValueTree& clip)
{
    if (!clip.isValid())
        return;
    const double start = clip[ids::loopStart], end = clip[ids::loopEnd];
    session.beginEdit("Halve loop", clip);
    session.set().setClipLoop(clip, start, start + juce::jmax(LiveSet::minLoopLength, (end - start) * 0.5));
}

void duplicateLoop(SessionController& session, const juce::ValueTree& clip)
{
    if (!clip.isValid())
        return;
    const double start = clip[ids::loopStart], end = clip[ids::loopEnd];
    session.beginEdit("Duplicate loop", clip);
    notes::apply(session.set(), clip, notes::duplicateLoop(LiveSet::notesOf(clip), start, end));
    session.set().setClipLoop(clip, start, start + (end - start) * 2.0);
}

void quantize(SessionController& session, const juce::ValueTree& clip, double gridStep, double amount)
{
    if (!clip.isValid())
        return;
    const auto all = LiveSet::notesOf(clip);
    const auto change = notes::quantize(all, notes::all(all), gridStep, amount, false);
    if (change.empty())
        return;
    session.beginEdit("Quantize", clip);
    notes::apply(session.set(), clip, change);
}

void quantize(SessionController& session, const juce::ValueTree& clip)
{
    const auto& set = session.set();
    quantize(session, clip, grids()[static_cast<std::size_t>(quantizeGridOf(set))].second, quantizeAmountOf(set) / 100.0);
}

const std::vector<std::pair<juce::String, double>>& grids()
{
    static const std::vector<std::pair<juce::String, double>> list{
        {"1/4", 1.0}, {"1/8", 0.5}, {"1/8T", 1.0 / 3.0}, {"1/16", 0.25}, {"1/16T", 1.0 / 6.0}, {"1/32", 0.125}};
    return list;
}

int quantizeGridOf(const LiveSet& set)
{
    return juce::jlimit(0, static_cast<int>(grids().size()) - 1, static_cast<int>(set.state().getProperty(ids::quantizeGrid, defaultGrid)));
}

int quantizeAmountOf(const LiveSet& set)
{
    return juce::jlimit(0, 100, static_cast<int>(set.state().getProperty(ids::quantizeAmount, 100)));
}

const std::vector<int>& fixedLengths()
{
    static const std::vector<int> list{0, 1, 2, 4, 8, 16};
    return list;
}

std::unique_ptr<ValueBinding> setBinding(LiveSet& set, juce::UndoManager* undo, const juce::String& property)
{
    auto root = set.state();
    const auto only = [](const juce::Identifier& id) {
        return [id](const juce::ValueTree& tree, const juce::Identifier& p) { return tree.hasType(ids::liveSet) && p == id; };
    };
    std::unique_ptr<ValueBinding> b;
    if (property == "quantizeGrid")
    {
        juce::StringArray names;
        for (const auto& [name, length] : grids())
            names.add(name);
        // A setting of the controllers, not an edit of the set: no undo step.
        b = std::make_unique<ValueBinding>(
            ParameterSpec::choice("quantizeGrid", "Quantize To", names, defaultGrid).inGroup("Quantize"),
            [&set] { return static_cast<double>(quantizeGridOf(set)); },
            [root](double v) mutable { root.setProperty(ids::quantizeGrid, juce::roundToInt(v), nullptr); }, nullptr, root,
            only(ids::quantizeGrid));
    }
    else if (property == "quantizeAmount")
    {
        b = std::make_unique<ValueBinding>(
            ParameterSpec::integer("quantizeAmount", "Quantize Amount", 0, 100, 100, ParamUnit::Percent).inGroup("Quantize"),
            [&set] { return static_cast<double>(quantizeAmountOf(set)); },
            [root](double v) mutable { root.setProperty(ids::quantizeAmount, juce::roundToInt(v), nullptr); }, nullptr, root,
            only(ids::quantizeAmount));
    }
    else if (property == "fixedRecordBars")
    {
        juce::StringArray names;
        for (const int bars : fixedLengths())
            names.add(bars == 0 ? juce::String("Off") : juce::String(bars) + (bars == 1 ? " bar" : " bars"));
        b = std::make_unique<ValueBinding>(
            ParameterSpec::choice("fixedRecordBars", "Fixed Length", names, 0).inGroup("Record"),
            [&set] {
                const auto& lengths = fixedLengths();
                const auto at = std::find(lengths.begin(), lengths.end(), set.fixedRecordBars());
                return static_cast<double>(at != lengths.end() ? at - lengths.begin() : 0);
            },
            [&set](double v) { set.setFixedRecordBars(fixedLengths()[static_cast<std::size_t>(juce::jlimit(0, 5, juce::roundToInt(v)))]); },
            undo, root, only(ids::fixedRecordBars));
    }
    if (b != nullptr)
        b->target = "set/" + property;
    return b;
}

} // namespace lattice::ui::clipActions
