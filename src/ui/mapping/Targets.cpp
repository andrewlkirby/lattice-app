#include "ui/mapping/Targets.h"

#include "ui/session/MixerStrip.h"

namespace lattice::ui
{

juce::ValueTree findDeviceById(const juce::ValueTree& node, std::int64_t id)
{
    if (node.hasType(ids::device) && LiveSet::idOf(node) == id)
        return node;
    for (const auto& child : node)
        if (auto found = findDeviceById(child, id); found.isValid())
            return found;
    return {};
}

std::unique_ptr<ValueBinding> bindingForTarget(LiveSet& set, DeviceSpecs& specs, juce::UndoManager* undo,
                                               const juce::String& target)
{
    std::unique_ptr<ValueBinding> made;
    const auto afterColon = target.fromFirstOccurrenceOf(":", false, false);
    const auto id = afterColon.upToFirstOccurrenceOf("/", false, false).getLargeIntValue();
    const auto what = target.fromLastOccurrenceOf("/", false, false);
    if (target.startsWith("device:"))
    {
        const auto device = findDeviceById(set.state(), id);
        if (const auto* spec = device.isValid() ? specs.forType(device[ids::deviceType].toString()) : nullptr)
            if (const auto* parameter = spec->findParameter(what))
                made = ValueBinding::deviceParameter(set, device, parameter->spec());
    }
    else if (target.startsWith("send:"))
    {
        const auto returnId = what.getLargeIntValue();
        const int r = set.returnIndexForId(returnId);
        int t = -1;
        for (int i = 0; i < set.numTracks(); ++i)
            if (set.trackId(i) == id)
                t = i;
        if (t >= 0 && r >= 0)
            made = std::make_unique<ValueBinding>(
                MixerStrip::sendSpec(set.returnTrack(r)[ids::name].toString()), [&set, t, r] { return set.sendLevel(t, r); },
                [&set, t, r](double db) { set.setSendLevel(t, r, db); }, undo, set.track(t),
                [](const juce::ValueTree& tree, const juce::Identifier&) { return tree.hasType(ids::send); });
    }
    else if (target.startsWith("node:") || target.startsWith("master/"))
    {
        juce::ValueTree node;
        int track = -1, ret = -1;
        if (target.startsWith("master/"))
            node = set.master();
        else
        {
            for (int i = 0; i < set.numTracks(); ++i)
                if (set.trackId(i) == id)
                {
                    track = i;
                    node = set.track(i);
                }
            if (!node.isValid() && (ret = set.returnIndexForId(id)) >= 0)
                node = set.returnTrack(ret);
        }
        if (node.isValid())
        {
            const juce::Identifier property(what);
            // Through the set's own setters, so its rules apply (solo and arm clear the others).
            const auto setter = [&set, track, ret, property](double v) {
                const bool on = v > 0.5;
                if (property == ids::volumeDb)
                {
                    if (track >= 0)
                        set.setTrackVolumeDb(track, v);
                    else if (ret >= 0)
                        set.setReturnVolumeDb(ret, v);
                    else
                        set.setMasterVolumeDb(v);
                }
                else if (property == ids::pan)
                {
                    if (track >= 0)
                        set.setTrackPan(track, v);
                    else if (ret >= 0)
                        set.setReturnPan(ret, v);
                    else
                        set.setMasterPan(v);
                }
                else if (property == ids::mute && track >= 0)
                    set.setTrackMute(track, on);
                else if (property == ids::mute && ret >= 0)
                    set.setReturnMute(ret, on);
                else if (property == ids::solo && track >= 0)
                    set.setTrackSolo(track, on, false);
                else if (property == ids::solo && ret >= 0)
                    set.setReturnSolo(ret, on);
                else if (property == ids::arm && track >= 0)
                    set.setTrackArm(track, on, false);
                else if (property == ids::preFader && ret >= 0)
                    set.setReturnPreFader(ret, on);
            };
            if (property == ids::volumeDb)
                made = ValueBinding::property(MixerStrip::volumeSpec(), undo, node, property, setter);
            else if (property == ids::pan)
                made = ValueBinding::property(MixerStrip::panSpec(), undo, node, property, setter);
            else if (property == ids::mute || property == ids::solo || property == ids::arm || property == ids::preFader)
                made = ValueBinding::property(ParameterSpec::toggle(what, what, false), undo, node, property, setter);
        }
    }
    return made;
}

} // namespace lattice::ui
