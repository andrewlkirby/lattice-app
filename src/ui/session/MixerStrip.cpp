#include "ui/session/MixerStrip.h"

#include "ui/theme/LatticeLookAndFeel.h"

#include <cmath>

namespace lattice::ui
{
namespace
{
ParameterSpec volumeSpec(float maxDb)
{
    return ParameterSpec::continuous("volumeDb", "Volume", static_cast<float>(LiveSet::minVolumeDb), maxDb, 0.0f, ParamUnit::Decibels)
        .withCentre(-12.0f)
        .inGroup("Mixer");
}

ParameterSpec panSpec()
{
    return ParameterSpec::continuous("pan", "Pan", -1.0f, 1.0f, 0.0f)
        .inGroup("Mixer")
        .withFormatter([](float v) {
            const int amount = juce::roundToInt(std::abs(v) * 50.0f);
            return amount == 0 ? juce::String("C") : juce::String(amount) + (v < 0.0f ? "L" : "R");
        });
}

/** Does any device in the set take its sidechain from this track? */
bool feedsASidechain(const LiveSet& set, std::int64_t trackId)
{
    std::function<bool(const juce::ValueTree&)> search = [&](const juce::ValueTree& node) {
        if (node.hasType(ids::deviceState) &&
            static_cast<juce::int64>(node.getProperty(ids::sidechainTrack, static_cast<juce::int64>(0))) == trackId)
            return true;
        for (const auto& child : node)
            if (search(child))
                return true;
        return false;
    };
    return trackId != 0 && search(set.state());
}

/** A toggle bound to a flag of a track (mute shown as "on", solo, arm). */
std::unique_ptr<Toggle> flagToggle(const juce::String& name, Toggle::Role role, const juce::String& caption,
                                   juce::UndoManager* undo, juce::ValueTree node, juce::Identifier id, bool inverted,
                                   std::function<void(bool)> set)
{
    auto binding = std::make_unique<ValueBinding>(
        ParameterSpec::toggle(id.toString(), name, inverted).inGroup("Mixer"),
        [node, id, inverted] { return static_cast<bool>(node.getProperty(id, false)) != inverted ? 1.0 : 0.0; },
        [set = std::move(set), inverted](double v) { set((v > 0.5) != inverted); }, undo, node,
        [node, id](const juce::ValueTree& tree, const juce::Identifier& property) { return tree == node && property == id; });
    return std::make_unique<Toggle>(std::move(binding), role, caption);
}
} // namespace

MixerStrip::MixerStrip(SessionController& s, Kind k, int i) : session(s), stripKind(k), stripIndex(i)
{
    auto& set = session.set();
    auto* undo = &session.undoManager();
    const auto node = k == Kind::track ? set.track(i) : k == Kind::returnTrack ? set.returnTrack(i) : set.master();
    setTitle(k == Kind::master ? juce::String("Master") : node[ids::name].toString());

    // Sends: a track's, to each return.
    if (k == Kind::track)
        for (int r = 0; r < set.numReturns(); ++r)
        {
            const auto returnName = set.returnTrack(r)[ids::name].toString();
            auto spec = ParameterSpec::continuous("send" + juce::String(r), "Send to " + returnName,
                                                  static_cast<float>(LiveSet::minVolumeDb), 0.0f,
                                                  static_cast<float>(LiveSet::minVolumeDb), ParamUnit::Decibels)
                            .withCentre(-18.0f)
                            .inGroup("Sends");
            auto binding = std::make_unique<ValueBinding>(
                std::move(spec), [&set, i, r] { return set.sendLevel(i, r); }, [&set, i, r](double db) { set.setSendLevel(i, r, db); },
                undo, node, [](const juce::ValueTree& tree, const juce::Identifier&) { return tree.hasType(ids::send); });
            auto knob = std::make_unique<Knob>(std::move(binding));
            knob->setLabel(returnName.substring(0, 1)); // the return's letter; the value shows in dB
            knob->setTitle("Send to " + returnName);
            addAndMakeVisible(*knob);
            sends.push_back(std::move(knob));
        }

    // Pan, volume.
    const auto setPan = [&set, k, i](double v) {
        if (k == Kind::track)
            set.setTrackPan(i, v);
        else if (k == Kind::returnTrack)
            set.setReturnPan(i, v);
        else
            set.setMasterPan(v);
    };
    const auto setVolume = [&set, k, i](double v) {
        if (k == Kind::track)
            set.setTrackVolumeDb(i, v);
        else if (k == Kind::returnTrack)
            set.setReturnVolumeDb(i, v);
        else
            set.setMasterVolumeDb(v);
    };
    pan = std::make_unique<Knob>(ValueBinding::property(panSpec(), undo, node, ids::pan, setPan));
    pan->setLabel({});
    volume = std::make_unique<Fader>(ValueBinding::property(volumeSpec(static_cast<float>(LiveSet::maxVolumeDb)), undo, node,
                                                            ids::volumeDb, setVolume));
    volumeText = std::make_unique<NumberBox>(ValueBinding::property(volumeSpec(static_cast<float>(LiveSet::maxVolumeDb)), undo,
                                                                    node, ids::volumeDb, setVolume));
    for (juce::Component* c : {static_cast<juce::Component*>(pan.get()), static_cast<juce::Component*>(volume.get()),
                               static_cast<juce::Component*>(volumeText.get())})
        addAndMakeVisible(*c);

    // The buttons.
    if (k == Kind::track)
    {
        on = flagToggle("Track On", Toggle::Role::mute, juce::String(i + 1), undo, node, ids::mute, true,
                        [&set, i](bool mute) { set.setTrackMute(i, mute); });
        solo = flagToggle("Solo", Toggle::Role::solo, "S", undo, node, ids::solo, false, [&set, i](bool v) {
            set.setTrackSolo(i, v, juce::ModifierKeys::currentModifiers.isCommandDown());
        });
        if (!set.isGroupTrack(i))
            arm = flagToggle("Arm", Toggle::Role::arm, juce::String::fromUTF8("\xe2\x97\x8f"), undo, node, ids::arm, false,
                             [&set, i](bool v) { set.setTrackArm(i, v, juce::ModifierKeys::currentModifiers.isCommandDown()); });
        sidechainBadge = feedsASidechain(set, LiveSet::idOf(node));
    }
    else if (k == Kind::returnTrack)
    {
        on = flagToggle("Return On", Toggle::Role::mute, juce::String::charToString(static_cast<juce::juce_wchar>('A' + i)), undo,
                        node, ids::mute, true, [&set, i](bool mute) { set.setReturnMute(i, mute); });
        solo = flagToggle("Solo", Toggle::Role::solo, "S", undo, node, ids::solo, false,
                          [&set, i](bool v) { set.setReturnSolo(i, v); });
        pre = flagToggle("Sends Before Faders", Toggle::Role::accent, "Pre", undo, node, ids::preFader, false,
                         [&set, i](bool v) { set.setReturnPreFader(i, v); });
    }
    for (auto* t : {on.get(), solo.get(), arm.get(), pre.get()})
        if (t != nullptr)
            addAndMakeVisible(*t);
}

void MixerStrip::setShowsSends(bool show)
{
    showSends = show;
    for (auto& s : sends)
        s->setVisible(show);
    resized();
}

void MixerStrip::showLevels(std::array<float, 2> peaks, double frameSeconds)
{
    auto& meter = volume->meter();
    meter.advance(frameSeconds);
    // The engine reports peaks; the solid bar shows them too until it reports RMS.
    meter.setLevels(0, peaks[0], peaks[0]);
    meter.setLevels(1, peaks[1], peaks[1]);
}

int MixerStrip::heightFor(const juce::Component& themed, int numReturns, bool withSends)
{
    const auto& t = themeOf(themed);
    const float knob = t.controlHeight * 1.6f + 2.0f * (t.fontSmall + 4.0f) + t.spacing;
    const float sendsHeight = withSends ? static_cast<float>(numReturns) * (knob * 0.75f) : 0.0f;
    const float main = knob * 0.8f + t.controlHeight * 7.0f + t.controlHeight + t.spacing * 4.0f;
    return juce::roundToInt(sendsHeight + main + t.padding * 2.0f);
}

void MixerStrip::resized()
{
    const auto& t = themeOf(*this);
    auto area = getLocalBounds().reduced(juce::roundToInt(t.padding * 0.5f), juce::roundToInt(t.padding * 0.5f));
    const int gap = juce::roundToInt(t.spacing);
    const int h = juce::roundToInt(t.controlHeight);
    const int knobHeight = juce::roundToInt(t.controlHeight * 1.6f + 2.0f * (t.fontSmall + 4.0f) + t.spacing);

    if (showSends)
        for (auto& s : sends)
            s->setBounds(area.removeFromTop(knobHeight * 3 / 4).withSizeKeepingCentre(juce::jmin(area.getWidth(), knobHeight), knobHeight * 3 / 4));

    pan->setBounds(area.removeFromTop(knobHeight * 4 / 5).withSizeKeepingCentre(juce::jmin(area.getWidth(), knobHeight), knobHeight * 4 / 5));
    area.removeFromTop(gap);

    auto buttons = area.removeFromBottom(h);
    area.removeFromBottom(gap);
    std::vector<Toggle*> shown;
    for (auto* b : {on.get(), solo.get(), arm.get(), pre.get()})
        if (b != nullptr)
            shown.push_back(b);
    const int w = shown.empty() ? 0 : buttons.getWidth() / static_cast<int>(shown.size());
    for (auto* b : shown)
        b->setBounds(buttons.removeFromLeft(w).reduced(1, 0));

    volumeText->setBounds(area.removeFromBottom(h).reduced(juce::roundToInt(t.padding * 0.5f), 0));
    area.removeFromBottom(gap);
    volume->setBounds(area.withSizeKeepingCentre(juce::jmin(area.getWidth(), h * 2), area.getHeight()));
}

void MixerStrip::paint(juce::Graphics& g)
{
    const auto& t = themeOf(*this);
    g.setColour(stripKind == Kind::master ? t.surfaceRaised : t.surface);
    g.fillRect(getLocalBounds().reduced(1, 0));
    if (sidechainBadge)
    {
        const auto badge = getLocalBounds().removeFromTop(juce::roundToInt(t.fontSmall + 4.0f)).removeFromRight(juce::roundToInt(t.controlHeight * 1.2f));
        g.setColour(t.accent);
        g.fillRoundedRectangle(badge.reduced(2).toFloat(), t.radius);
        g.setColour(t.accentText);
        g.setFont(t.font(t.fontSmall));
        g.drawText("SC", badge, juce::Justification::centred);
    }
}

} // namespace lattice::ui
