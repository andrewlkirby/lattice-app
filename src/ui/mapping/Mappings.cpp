#include "ui/mapping/Mappings.h"

#include "core/model/UndoNames.h"

#include "ui/session/MixerStrip.h"

#include <cmath>
#include <functional>

namespace lattice::ui
{
namespace
{
const juce::Identifier kindId{"kind"}, inputId{"input"}, channelId{"channel"}, numberId{"number"}, noteId{"note"}, keyId{"key"},
    targetId{"target"}, nameId{"name"}, minId{"min"}, maxId{"max"}, takeoverId{"takeover"}, encoderId{"encoder"}, uidId{"uid"};

/** The device node with a document ID, anywhere in the set (racks' chains too). */
juce::ValueTree findDevice(const juce::ValueTree& node, std::int64_t id)
{
    if (node.hasType(ids::device) && LiveSet::idOf(node) == id)
        return node;
    for (const auto& child : node)
        if (auto found = findDevice(child, id); found.isValid())
            return found;
    return {};
}
} // namespace

const juce::Identifier Mappings::mappingsType{"MAPPINGS"};
const juce::Identifier Mappings::mappingType{"MAPPING"};

Mappings::Mappings(SessionController& s, CommandRegistry& c, DeviceSpecs& d) : session(s), commands(c), specs(d) {}

Mappings::~Mappings()
{
    endGestures();
}

juce::ValueTree Mappings::list(bool create)
{
    auto root = session.set().state();
    auto node = root.getChildWithName(mappingsType);
    if (!node.isValid() && create)
    {
        node = juce::ValueTree(mappingsType);
        root.appendChild(node, session.set().undoManager());
    }
    return node;
}

std::vector<juce::ValueTree> Mappings::all() const
{
    std::vector<juce::ValueTree> out;
    for (const auto& m : session.set().state().getChildWithName(mappingsType))
        out.push_back(m);
    return out;
}

std::vector<juce::ValueTree> Mappings::forTarget(const juce::String& target) const
{
    auto out = all();
    std::erase_if(out, [&](const juce::ValueTree& m) { return m[targetId].toString() != target; });
    return out;
}

void Mappings::setMode(Mode m)
{
    current = m;
    learning = {};
    changed();
}

void Mappings::setLearnTarget(const juce::String& target, const juce::String& name)
{
    learning = target;
    learningName = name;
    changed();
}

void Mappings::changed()
{
    if (onChange)
        onChange();
}

// --- Sources -------------------------------------------------------------------------------------

Mappings::Source Mappings::sourceOf(const juce::ValueTree& m)
{
    Source s;
    s.key = m[kindId].toString() == "key";
    s.input = m[inputId].toString();
    s.channel = m[channelId];
    s.number = m[numberId];
    s.note = m[noteId];
    s.keyText = m[keyId].toString();
    return s;
}

bool Mappings::sameSource(const Source& a, const Source& b)
{
    if (a.key != b.key)
        return false;
    if (a.key)
        return a.keyText == b.keyText;
    return a.channel == b.channel && a.number == b.number && a.note == b.note && (a.input.isEmpty() || b.input.isEmpty() || a.input == b.input);
}

juce::String Mappings::sourceText(const juce::ValueTree& m)
{
    const auto s = sourceOf(m);
    if (s.key)
        return "Key " + juce::KeyPress::createFromDescription(s.keyText).getTextDescriptionWithIcons();
    const auto what = s.note ? "Note " + juce::MidiMessage::getMidiNoteName(s.number, true, true, 3) : "CC " + juce::String(s.number);
    return what + juce::String::fromUTF8(" \xc2\xb7 Ch ") + juce::String(s.channel);
}

void Mappings::beginEdit(const juce::String& action, const juce::String& target)
{
    // Where the target is, as its control's own steps say; a clip slot, scene or command has none.
    auto* binding = bindingFor(target);
    session.undoManager().beginNewTransaction(undoNames::make(action, binding != nullptr && binding->location ? binding->location() : juce::String()));
}

void Mappings::learn(const Source& source)
{
    if (learning.isEmpty())
        return;
    auto& undo = session.undoManager();
    beginEdit((source.key ? "Key Map " : "MIDI Map ") + learningName, learning);
    auto node = list(true);
    // One source plays one target, and a target has one MIDI and one key mapping: either one
    // already there is replaced.
    for (int i = node.getNumChildren(); --i >= 0;)
    {
        const auto m = node.getChild(i);
        const auto s = sourceOf(m);
        if (sameSource(s, source) || (m[targetId].toString() == learning && s.key == source.key))
            node.removeChild(i, &undo);
    }
    juce::ValueTree m(mappingType);
    m.setProperty(kindId, source.key ? "key" : "midi", nullptr);
    if (source.key)
        m.setProperty(keyId, source.keyText, nullptr);
    else
    {
        m.setProperty(inputId, source.input, nullptr);
        m.setProperty(channelId, source.channel, nullptr);
        m.setProperty(numberId, source.number, nullptr);
        m.setProperty(noteId, source.note, nullptr);
    }
    m.setProperty(targetId, learning, nullptr);
    m.setProperty(nameId, learningName, nullptr);
    m.setProperty(minId, 0.0, nullptr);
    m.setProperty(maxId, 1.0, nullptr);
    m.setProperty(takeoverId, static_cast<int>(Takeover::pickup), nullptr);
    m.setProperty(encoderId, static_cast<int>(Encoder::absolute), nullptr);
    m.setProperty(uidId, juce::Uuid().toString(), nullptr);
    node.appendChild(m, &undo);
    changed();
}

void Mappings::remove(const juce::ValueTree& mapping)
{
    auto node = list(false);
    if (!node.isValid() || mapping.getParent() != node)
        return;
    beginEdit("Remove Mapping of " + mapping[nameId].toString(), mapping[targetId].toString());
    node.removeChild(mapping, &session.undoManager());
    changed();
}

void Mappings::setRange(const juce::ValueTree& mapping, double min, double max)
{
    auto m = mapping;
    beginEdit("Mapping Range of " + mapping[nameId].toString(), mapping[targetId].toString());
    m.setProperty(minId, juce::jlimit(0.0, 1.0, min), &session.undoManager());
    m.setProperty(maxId, juce::jlimit(0.0, 1.0, max), &session.undoManager());
    changed();
}

void Mappings::setTakeover(const juce::ValueTree& mapping, Takeover t)
{
    auto m = mapping;
    beginEdit("Mapping Takeover of " + mapping[nameId].toString(), mapping[targetId].toString());
    m.setProperty(takeoverId, static_cast<int>(t), &session.undoManager());
    changed();
}

void Mappings::setEncoder(const juce::ValueTree& mapping, Encoder e)
{
    auto m = mapping;
    beginEdit("Mapping Encoder of " + mapping[nameId].toString(), mapping[targetId].toString());
    m.setProperty(encoderId, static_cast<int>(e), &session.undoManager());
    changed();
}

// --- Input ---------------------------------------------------------------------------------------

bool Mappings::handleMidi(const juce::String& input, const juce::MidiMessage& message)
{
    const bool cc = message.isController();
    const bool noteOn = message.isNoteOn();
    const bool noteOff = message.isNoteOff();
    if (!cc && !noteOn && !noteOff)
        return false;
    Source source;
    source.input = input;
    source.channel = message.getChannel();
    source.note = !cc;
    source.number = cc ? message.getControllerNumber() : message.getNoteNumber();
    if (current == Mode::midi)
    {
        if (noteOff || learning.isEmpty())
            return noteOff; // a note's end isn't learnt
        learn(source);
        return true;
    }
    bool used = false;
    for (const auto& m : all())
        if (!sourceOf(m).key && sameSource(sourceOf(m), source))
        {
            play(m, cc ? message.getControllerValue() : (noteOn ? message.getVelocity() : 0), !cc);
            used = true;
        }
    return used;
}

bool Mappings::handleKey(const juce::KeyPress& key)
{
    if (isKeyboardKey && isKeyboardKey(key))
        return false; // the computer keyboard plays it
    Source source;
    source.key = true;
    source.keyText = key.getTextDescription();
    if (current == Mode::key)
    {
        if (learning.isEmpty())
            return false;
        learn(source);
        return true;
    }
    if (current != Mode::off)
        return false;
    bool used = false;
    for (const auto& m : all())
        if (sameSource(sourceOf(m), source))
        {
            play(m, 127, true);
            play(m, 0, true);
            used = true;
        }
    return used;
}

// --- Playing -------------------------------------------------------------------------------------

bool Mappings::press(const juce::String& target, bool down)
{
    auto& set = session.set();
    const auto trackIndexOf = [&set](std::int64_t id) {
        for (int t = 0; t < set.numTracks(); ++t)
            if (set.trackId(t) == id)
                return t;
        return -1;
    };
    if (target.startsWith("slot:"))
    {
        const auto t = trackIndexOf(target.fromFirstOccurrenceOf(":", false, false).upToFirstOccurrenceOf("/", false, false).getLargeIntValue());
        const int scene = target.fromLastOccurrenceOf("/", false, false).getIntValue();
        if (t < 0 || scene < 0 || scene >= set.numScenes())
            return true;
        if (down)
            session.launchSlot(t, scene);
        else
            session.releaseSlot(t, scene);
        return true;
    }
    if (target.startsWith("scene:"))
    {
        const auto id = target.fromFirstOccurrenceOf(":", false, false).getLargeIntValue();
        for (int s = 0; s < set.numScenes(); ++s)
            if (LiveSet::idOf(set.scene(s)) == id && down)
                session.launchScene(s);
        return true;
    }
    if (target.startsWith("command:"))
    {
        if (down)
            commands.perform(target.fromFirstOccurrenceOf(":", false, false));
        return true;
    }
    return false;
}

void Mappings::play(const juce::ValueTree& m, int value, bool isNote)
{
    const auto target = m[targetId].toString();
    if (press(target, value > 0))
        return;
    auto* binding = bindingFor(target);
    if (binding == nullptr)
        return;
    const double lo = m[minId], hi = m[maxId];
    auto& held = state[m[uidId].toString()];
    const auto now = juce::Time::currentTimeMillis();
    const auto setTo = [&](double normalised) {
        if (!binding->inGesture())
            binding->beginGesture();
        binding->setNormalised(static_cast<float>(juce::jlimit(0.0, 1.0, normalised)));
        held.touched = now;
    };
    if (isNote)
    {
        if (value <= 0)
            return; // a note's end
        // Toggles between the range's ends: to whichever the value is further from.
        const double v = binding->normalised();
        const double to = std::abs(v - hi) > std::abs(v - lo) ? hi : lo;
        binding->setNormalised(static_cast<float>(to)); // one step of its own
        return;
    }
    const auto encoder = static_cast<Encoder>(static_cast<int>(m[encoderId]));
    if (encoder != Encoder::absolute)
    {
        int steps = 0;
        if (encoder == Encoder::twosComplement)
            steps = value < 64 ? value : value - 128;
        else if (encoder == Encoder::binaryOffset)
            steps = value - 64;
        else
            steps = value < 64 ? value : -(value - 64);
        const double v = binding->normalised();
        setTo(juce::jlimit(juce::jmin(lo, hi), juce::jmax(lo, hi), v + steps * (hi - lo) / 127.0));
        return;
    }
    const double c = value / 127.0;          // the control
    const double want = lo + c * (hi - lo);  // where it says the value should be
    const double v = binding->normalised();  // where the value is
    const auto takeover = static_cast<Takeover>(static_cast<int>(m[takeoverId]));
    if (takeover == Takeover::pickup)
    {
        // Nothing until the control reaches the value (or passes it between two messages).
        const double tolerance = std::abs(hi - lo) / 127.0 * 1.5;
        const double wasAt = held.last >= 0.0 ? lo + held.last * (hi - lo) : want;
        if (!held.picked && (std::abs(want - v) <= tolerance || (wasAt - v) * (want - v) < 0.0))
            held.picked = true;
        held.last = c;
        if (held.picked)
            setTo(want);
        return;
    }
    // Value scaling: the value moves the same way as the control, so that they meet at the ends.
    if (held.last < 0.0 || hi == lo)
    {
        held.last = c;
        return;
    }
    const double here = (v - lo) / (hi - lo); // the value, along the control's travel
    double next = here;
    if (c > held.last)
        next = here + (c - held.last) * (1.0 - here) / juce::jmax(1e-9, 1.0 - held.last);
    else if (c < held.last)
        next = here - (held.last - c) * here / juce::jmax(1e-9, held.last);
    held.last = c;
    setTo(lo + juce::jlimit(0.0, 1.0, next) * (hi - lo));
}

ValueBinding* Mappings::bindingFor(const juce::String& target)
{
    auto& set = session.set();
    if (auto it = bindings.find(target); it != bindings.end())
    {
        // Still in the set (the device or track may have been deleted since, or another set loaded).
        const auto* binding = it->second.get();
        if (binding != nullptr && (binding->watchedTree() == set.state() || binding->watchedTree().isAChildOf(set.state())))
            return it->second.get();
    }
    auto* undo = &session.undoManager();
    std::unique_ptr<ValueBinding> made;
    const auto afterColon = target.fromFirstOccurrenceOf(":", false, false);
    const auto id = afterColon.upToFirstOccurrenceOf("/", false, false).getLargeIntValue();
    const auto what = target.fromLastOccurrenceOf("/", false, false);
    if (target.startsWith("device:"))
    {
        const auto device = findDevice(set.state(), id);
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
    auto* raw = made.get();
    bindings[target] = std::move(made);
    return raw;
}

void Mappings::endGestures()
{
    for (auto& [target, binding] : bindings)
        if (binding != nullptr && binding->inGesture())
            binding->endGesture();
}

bool Mappings::tick()
{
    const auto now = juce::Time::currentTimeMillis();
    // A long rest: the control picks up the value again (checked before its next move, since the
    // ticks stop while nothing moves).
    for (auto& [uid, held] : state)
        held.picked = held.picked && now - held.touched < 10'000;
    bool moved = false;
    if (pullMidi)
        for (const auto& [input, message] : pullMidi())
        {
            handleMidi(input, message);
            moved = true;
        }
    // A control at rest has ended its gesture: the turn so far is one undo step. Bindings are
    // made again later (a device may have been deleted, or a set loaded).
    if (bindings.empty())
        return moved; // no gesture to end
    for (const auto& [uid, held] : state)
        if (now - held.touched < restMs)
            return true;
    endGestures();
    bindings.clear();
    return moved;
}

} // namespace lattice::ui
