#include "ui/tuning/TrackTuningControls.h"

#include "ui/theme/LatticeLookAndFeel.h"

namespace lattice::ui
{

TrackTuningControls::TrackTuningControls(SessionController& s, TuningRegistry& tunings, int t, Layout l)
    : session(s), registry(tunings), trackIndex(t), layout(l), node(s.set().track(t))
{
    setTitle("Tuning settings");
    auto& set = session.set();
    auto* undo = &session.undoManager();
    const int i = trackIndex;
    makeTuningBox();

    highNotes = std::make_unique<NumberBox>(ValueBinding::property(
        ParameterSpec::choice("highNotePolicy", "High Notes", {"Mute", "Fold Down"}, 0).inGroup("Tuning"), undo, node,
        ids::highNotePolicy, [&set, i](double v) { set.setTrackHighNotePolicy(i, juce::roundToInt(v)); }));
    highNotes->setTooltip("Notes too high to play at the sample rate: muted, or folded down by the tuning's period.");

    const auto n = node;
    bendMode = std::make_unique<NumberBox>(std::make_unique<ValueBinding>(
        ParameterSpec::choice("bendMode", "Bend", {"Cents", "Steps"}, 0).inGroup("Tuning"),
        [n] { return static_cast<double>(LiveSet::bendModeOf(n)); }, [&set, i](double v) { set.setTrackBendMode(i, juce::roundToInt(v)); },
        undo, node, [n](const juce::ValueTree& tree, const juce::Identifier& p) { return tree == n && p == ids::bendMode; }));
    bendMode->setTooltip("Pitch bend by a musical interval (Cents), or along the tuning's table (Steps).");

    rangeCents = std::make_unique<NumberBox>(std::make_unique<ValueBinding>(
        ParameterSpec::continuous("bendRangeCents", "Bend Range", 0.0f, static_cast<float>(LiveSet::maxBendRangeCents),
                                  static_cast<float>(LiveSet::defaultBendRangeCents), ParamUnit::Cents)
            .withInterval(1.0f)
            .inGroup("Tuning"),
        [n] { return LiveSet::bendRangeCentsOf(n); }, [&set, i](double v) { set.setTrackBendRangeCents(i, v); }, undo, node,
        [n](const juce::ValueTree& tree, const juce::Identifier& p) { return tree == n && p == ids::bendRangeCents; }));
    rangeSteps = std::make_unique<NumberBox>(std::make_unique<ValueBinding>(
        ParameterSpec::continuous("bendRangeSteps", "Bend Range (Steps)", 0.0f, static_cast<float>(LiveSet::maxBendRangeSteps),
                                  static_cast<float>(LiveSet::defaultBendRangeSteps), ParamUnit::TableSteps)
            .withInterval(0.01f)
            .inGroup("Tuning"),
        [n] { return LiveSet::bendRangeStepsOf(n); }, [&set, i](double v) { set.setTrackBendRangeSteps(i, v); }, undo, node,
        [n](const juce::ValueTree& tree, const juce::Identifier& p) { return tree == n && p == ids::bendRangeSteps; }));
    for (auto* box : {rangeCents.get(), rangeSteps.get()})
        box->setTooltip("How far a full pitch bend reaches.");

    for (auto* box : {highNotes.get(), bendMode.get(), rangeCents.get(), rangeSteps.get()})
        addAndMakeVisible(*box);
    rangeSteps->setVisible(stepsMode());
    rangeCents->setVisible(!stepsMode());

    registry.addListener(this);
    node.addListener(this);
}

TrackTuningControls::~TrackTuningControls()
{
    node.removeListener(this);
    registry.removeListener(this);
}

bool TrackTuningControls::stepsMode() const
{
    return LiveSet::bendModeOf(node) == 1;
}

void TrackTuningControls::makeTuningBox()
{
    // The registry's tunings, and the one the track names if the registry hasn't got it (a set
    // from another computer): shown as missing, so choosing another is a deliberate change.
    juce::StringArray tuningIds, names;
    for (const auto& t : registry.getAll())
    {
        tuningIds.add(t->id);
        names.add(t->name);
    }
    const auto current = node[ids::tuningId].toString();
    if (current.isNotEmpty() && !tuningIds.contains(current))
    {
        const auto known = registry.findById(current); // an older version of a file in the list
        tuningIds.add(current);
        names.add(known != nullptr ? known->name + " (older version)" : current + " (missing)");
    }
    auto& set = session.set();
    const int i = trackIndex;
    const auto n = node;
    const auto fallback = registry.defaultTuning() != nullptr ? registry.defaultTuning()->id : juce::String();
    auto binding = std::make_unique<ValueBinding>(
        ParameterSpec::choice("tuning", "Tuning", names, 0).inGroup("Tuning"),
        [n, tuningIds, fallback] {
            const auto id = n[ids::tuningId].toString();
            return static_cast<double>(juce::jmax(0, tuningIds.indexOf(id.isEmpty() ? fallback : id)));
        },
        [&set, i, tuningIds](double v) { set.setTrackTuning(i, tuningIds[juce::jlimit(0, tuningIds.size() - 1, juce::roundToInt(v))]); },
        &session.undoManager(), node, [n](const juce::ValueTree& tree, const juce::Identifier& p) { return tree == n && p == ids::tuningId; });
    tuning = std::make_unique<NumberBox>(std::move(binding));
    tuning->setTooltip("The track's tuning: what every instrument on it plays.");
    addAndMakeVisible(*tuning);
}

void TrackTuningControls::tuningsChanged()
{
    removeChildComponent(tuning.get());
    tuning.reset();
    makeTuningBox();
    resized();
}

void TrackTuningControls::valueTreePropertyChanged(juce::ValueTree& tree, const juce::Identifier& property)
{
    if (tree != node)
        return;
    if (property == ids::bendMode)
    {
        rangeSteps->setVisible(stepsMode());
        rangeCents->setVisible(!stepsMode());
    }
    else if (property == ids::tuningId && !registry.getAll().empty())
    {
        // A tuning the list doesn't have (an older version, a missing file) needs its own entry.
        const auto id = tree[ids::tuningId].toString();
        bool listed = id.isEmpty();
        for (const auto& t : registry.getAll())
            listed = listed || t->id == id;
        if (!listed)
            tuningsChanged();
    }
}

int TrackTuningControls::rowWidth() const
{
    const auto& t = themeOf(*this);
    return juce::roundToInt(t.controlHeight * (6.0f + 4.0f + 3.0f + 3.0f) + t.spacing * 3.0f);
}

void TrackTuningControls::resized()
{
    const auto& t = themeOf(*this);
    auto area = getLocalBounds();
    const int gap = juce::roundToInt(t.spacing);
    const int h = juce::roundToInt(t.controlHeight);
    if (layout == Layout::stacked)
    {
        tuning->setBounds(area.removeFromTop(h));
        area.removeFromTop(gap);
        highNotes->setBounds(area.removeFromTop(h));
        area.removeFromTop(gap);
        auto row = area.removeFromTop(h);
        bendMode->setBounds(row.removeFromLeft(row.getWidth() / 2).withTrimmedRight(1));
        rangeCents->setBounds(row.withTrimmedLeft(1));
        rangeSteps->setBounds(row.withTrimmedLeft(1));
        return;
    }
    const auto take = [&](float units) {
        auto r = area.removeFromLeft(juce::roundToInt(t.controlHeight * units));
        area.removeFromLeft(gap);
        return r.withSizeKeepingCentre(r.getWidth(), juce::jmin(h, r.getHeight()));
    };
    tuning->setBounds(take(6.0f));
    highNotes->setBounds(take(4.0f));
    bendMode->setBounds(take(3.0f));
    const auto range = take(3.0f);
    rangeCents->setBounds(range);
    rangeSteps->setBounds(range);
}

} // namespace lattice::ui
