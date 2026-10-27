#include "core/model/LiveSet.h"

#include <algorithm>
#include <array>
#include <functional>
#include <map>
#include <cmath>

namespace lattice
{
namespace
{
// A few Live-like clip/track colours (ARGB).
constexpr std::array<std::uint32_t, 12> palette{0xffff94a6, 0xffffa529, 0xffcc9927, 0xfff7f47c,
                                                0xffbffb00, 0xff1aff2f, 0xff25ffa8, 0xff5cffe8,
                                                0xff8bc5ff, 0xff5480e4, 0xff92a7ff, 0xffd86ce4};

juce::var colourVar(int index)
{
    return static_cast<juce::int64>(palette[static_cast<std::size_t>(index) % palette.size()]);
}

bool isValidDenominator(int den)
{
    return den == 1 || den == 2 || den == 4 || den == 8 || den == 16;
}
} // namespace

juce::String& LiveSet::defaultInstrumentType()
{
    static juce::String type{"lattice.synth"};
    return type;
}

LiveSet::LiveSet(juce::UndoManager* undoManager) : root(ids::liveSet), undo(undoManager)
{
    resetToDefault();
}

// --- Whole document ---------------------------------------------------------------------------------

void LiveSet::resetToDefault(int numTracksToCreate, int numScenesToCreate)
{
    juce::ValueTree fresh(ids::liveSet);
    fresh.setProperty(ids::formatVersion, currentFormatVersion, nullptr);
    fresh.setProperty(ids::nextId, static_cast<juce::int64>(1), nullptr);
    fresh.setProperty(ids::tempo, 120.0, nullptr);
    fresh.setProperty(ids::timeSigNum, 4, nullptr);
    fresh.setProperty(ids::timeSigDen, 4, nullptr);
    fresh.setProperty(ids::globalQuantization, static_cast<int>(Quantization::Bar1), nullptr);
    fresh.setProperty(ids::metronomeOn, false, nullptr);
    fresh.setProperty(ids::metronomeVolumeDb, -12.0, nullptr);
    fresh.setProperty(ids::selectNextSceneOnLaunch, true, nullptr);
    fresh.setProperty(ids::fixedRecordBars, 0, nullptr);
    fresh.appendChild(juce::ValueTree(ids::tracks), nullptr);
    fresh.appendChild(juce::ValueTree(ids::scenes), nullptr);
    fresh.appendChild(juce::ValueTree(ids::returns), nullptr);
    juce::ValueTree masterNode(ids::master);
    masterNode.setProperty(ids::volumeDb, 0.0, nullptr);
    masterNode.setProperty(ids::pan, 0.0, nullptr);
    masterNode.appendChild(juce::ValueTree(ids::devices), nullptr);
    fresh.appendChild(masterNode, nullptr);

    // Swap the content into the existing root so listeners stay attached.
    root.copyPropertiesAndChildrenFrom(fresh, nullptr);
    if (undo != nullptr)
        undo->clearUndoHistory();

    for (int r = 0; r < defaultReturns; ++r)
        returnsNode().appendChild(makeReturn(r + 1), nullptr);
    for (int s = 0; s < numScenesToCreate; ++s)
        scenesNode().appendChild(makeScene(), nullptr);
    for (int t = 0; t < numTracksToCreate; ++t)
        tracksNode().appendChild(makeTrack(t + 1), nullptr);
    if (numTracksToCreate > 0)
        track(0).setProperty(ids::arm, true, nullptr);

    if (undo != nullptr)
        undo->clearUndoHistory();
}

juce::Result LiveSet::loadFromXml(const juce::XmlElement& xml)
{
    auto loaded = juce::ValueTree::fromXml(xml);
    if (!loaded.isValid() || !loaded.hasType(ids::liveSet))
        return juce::Result::fail("This is not a Lattice set.");

    const int version = loaded.getProperty(ids::formatVersion, 0);
    if (version < 1)
        return juce::Result::fail("The set has no format version.");
    if (version > currentFormatVersion)
        return juce::Result::fail("The set was saved by a newer version of Lattice (format " +
                                  juce::String(version) + ").");
    // Older formats are converted after loading, one version step at a time (below).
    loaded.setProperty(ids::formatVersion, currentFormatVersion, nullptr);

    if (!loaded.getChildWithName(ids::tracks).isValid() || !loaded.getChildWithName(ids::scenes).isValid() ||
        !loaded.getChildWithName(ids::master).isValid())
        return juce::Result::fail("The set is missing its tracks, scenes or master.");

    // Every track must have one slot per scene.
    const int sceneCount = loaded.getChildWithName(ids::scenes).getNumChildren();
    for (const auto& t : loaded.getChildWithName(ids::tracks))
        if (t.getChildWithName(ids::slots).getNumChildren() != sceneCount)
            return juce::Result::fail("Track \"" + t[ids::name].toString() +
                                      "\" does not have one slot per scene.");

    root.copyPropertiesAndChildrenFrom(loaded, nullptr);

    // 1 -> 2: device chains. M2 tracks all played the same built-in instrument.
    for (auto t : tracksNode())
    {
        if (!t.getChildWithName(ids::devices).isValid())
        {
            juce::ValueTree chain(ids::devices);
            if (version < 2)
                chain.appendChild(makeDevice(defaultInstrumentType()), nullptr);
            t.appendChild(chain, nullptr);
        }
    }

    // 2 -> 3: returns (none in older sets, so they sound the same) and the master's chain.
    if (!root.getChildWithName(ids::returns).isValid())
        root.appendChild(juce::ValueTree(ids::returns), nullptr);
    if (!master().getChildWithName(ids::devices).isValid())
        master().appendChild(juce::ValueTree(ids::devices), nullptr);
    for (auto r : returnsNode())
        if (!r.getChildWithName(ids::devices).isValid())
            r.appendChild(juce::ValueTree(ids::devices), nullptr);

    if (undo != nullptr)
        undo->clearUndoHistory();
    return juce::Result::ok();
}

std::unique_ptr<juce::XmlElement> LiveSet::toXml() const
{
    return root.createXml();
}

juce::Result LiveSet::saveToFile(const juce::File& file) const
{
    const auto xml = toXml();
    if (xml == nullptr)
        return juce::Result::fail("Could not serialise the set.");

    // Write to a temporary file first so a failed save never destroys the previous one.
    juce::TemporaryFile temp(file);
    if (!xml->writeTo(temp.getFile()))
        return juce::Result::fail("Could not write " + temp.getFile().getFullPathName());
    if (!temp.overwriteTargetFileWithTemporary())
        return juce::Result::fail("Could not replace " + file.getFullPathName());
    return juce::Result::ok();
}

juce::Result LiveSet::loadFromFile(const juce::File& file)
{
    const auto xml = juce::XmlDocument::parse(file);
    if (xml == nullptr)
        return juce::Result::fail("Could not read " + file.getFullPathName());
    return loadFromXml(*xml);
}

// --- Set ----------------------------------------------------------------------------------------------

double LiveSet::tempo() const
{
    return root[ids::tempo];
}

void LiveSet::setTempo(double bpm, bool undoable)
{
    root.setProperty(ids::tempo, juce::jlimit(minTempo, maxTempo, bpm), um(undoable));
}

int LiveSet::timeSigNumerator() const
{
    return root[ids::timeSigNum];
}

int LiveSet::timeSigDenominator() const
{
    return root[ids::timeSigDen];
}

void LiveSet::setTimeSignature(int numerator, int denominator, bool undoable)
{
    if (numerator < 1 || numerator > 99 || !isValidDenominator(denominator))
        return;
    root.setProperty(ids::timeSigNum, numerator, um(undoable));
    root.setProperty(ids::timeSigDen, denominator, um(undoable));
}

double LiveSet::beatsPerBar() const
{
    return timeSigNumerator() * 4.0 / timeSigDenominator();
}

Quantization LiveSet::globalQuantization() const
{
    return static_cast<Quantization>(static_cast<int>(root[ids::globalQuantization]));
}

void LiveSet::setGlobalQuantization(Quantization q)
{
    if (q == Quantization::Global)
        return;
    root.setProperty(ids::globalQuantization, static_cast<int>(q), undo);
}

bool LiveSet::metronomeOn() const
{
    return root[ids::metronomeOn];
}

void LiveSet::setMetronomeOn(bool on)
{
    root.setProperty(ids::metronomeOn, on, undo);
}

double LiveSet::metronomeVolumeDb() const
{
    return root[ids::metronomeVolumeDb];
}

void LiveSet::setMetronomeVolumeDb(double db)
{
    root.setProperty(ids::metronomeVolumeDb, juce::jlimit(minVolumeDb, maxVolumeDb, db), undo);
}

bool LiveSet::selectNextSceneOnLaunch() const
{
    return root[ids::selectNextSceneOnLaunch];
}

void LiveSet::setSelectNextSceneOnLaunch(bool on)
{
    root.setProperty(ids::selectNextSceneOnLaunch, on, undo);
}

int LiveSet::fixedRecordBars() const
{
    return root[ids::fixedRecordBars];
}

void LiveSet::setFixedRecordBars(int bars)
{
    if (bars != 0 && bars != 1 && bars != 2 && bars != 4 && bars != 8 && bars != 16)
        return;
    root.setProperty(ids::fixedRecordBars, bars, undo);
}

// --- Tracks ---------------------------------------------------------------------------------------------

juce::ValueTree LiveSet::tracksNode() const
{
    return root.getChildWithName(ids::tracks);
}

juce::ValueTree LiveSet::scenesNode() const
{
    return root.getChildWithName(ids::scenes);
}

int LiveSet::numTracks() const
{
    return tracksNode().getNumChildren();
}

juce::ValueTree LiveSet::track(int index) const
{
    return tracksNode().getChild(index);
}

std::int64_t LiveSet::trackId(int index) const
{
    return idOf(track(index));
}

int LiveSet::trackIndexForId(std::int64_t trackIdToFind) const
{
    const auto node = tracksNode();
    for (int i = 0; i < node.getNumChildren(); ++i)
        if (idOf(node.getChild(i)) == trackIdToFind)
            return i;
    return -1;
}

std::int64_t LiveSet::idOf(const juce::ValueTree& node)
{
    return static_cast<juce::int64>(node[ids::id]);
}

// --- Return tracks and sends ---------------------------------------------------------------------------

juce::ValueTree LiveSet::returnsNode() const
{
    return root.getChildWithName(ids::returns);
}

juce::ValueTree LiveSet::makeReturn(int number)
{
    juce::ValueTree r(ids::returnTrack);
    r.setProperty(ids::id, static_cast<juce::int64>(allocateId()), nullptr);
    r.setProperty(ids::name, juce::String::charToString(static_cast<juce::juce_wchar>('A' + (number - 1) % 26)) + " Return", nullptr);
    r.setProperty(ids::colour, colourVar(number + 5), nullptr);
    r.setProperty(ids::volumeDb, 0.0, nullptr);
    r.setProperty(ids::pan, 0.0, nullptr);
    r.setProperty(ids::mute, false, nullptr);
    r.setProperty(ids::solo, false, nullptr);
    r.setProperty(ids::preFader, false, nullptr);
    r.appendChild(juce::ValueTree(ids::devices), nullptr);
    return r;
}

int LiveSet::numReturns() const
{
    return returnsNode().getNumChildren();
}

juce::ValueTree LiveSet::returnTrack(int index) const
{
    return returnsNode().getChild(index);
}

std::int64_t LiveSet::returnId(int index) const
{
    return idOf(returnTrack(index));
}

int LiveSet::returnIndexForId(std::int64_t id) const
{
    for (int i = 0; i < numReturns(); ++i)
        if (returnId(i) == id)
            return i;
    return -1;
}

int LiveSet::addReturn()
{
    if (numReturns() >= maxReturns)
        return -1;
    const int index = numReturns();
    returnsNode().appendChild(makeReturn(index + 1), undo);
    return index;
}

void LiveSet::deleteReturn(int index)
{
    if (index < 0 || index >= numReturns())
        return;
    const auto id = static_cast<juce::int64>(returnId(index));
    for (auto t : tracksNode())
    {
        auto sendsNode = t.getChildWithName(ids::sends);
        for (int i = sendsNode.getNumChildren(); --i >= 0;)
            if (static_cast<juce::int64>(sendsNode.getChild(i)[ids::returnId]) == id)
                sendsNode.removeChild(i, undo);
    }
    returnsNode().removeChild(index, undo);
}

void LiveSet::setReturnName(int index, const juce::String& name)
{
    returnTrack(index).setProperty(ids::name, name, undo);
}

void LiveSet::setReturnVolumeDb(int index, double db)
{
    returnTrack(index).setProperty(ids::volumeDb, juce::jlimit(minVolumeDb, maxVolumeDb, db), undo);
}

void LiveSet::setReturnPan(int index, double pan)
{
    returnTrack(index).setProperty(ids::pan, juce::jlimit(-1.0, 1.0, pan), undo);
}

void LiveSet::setReturnMute(int index, bool mute)
{
    returnTrack(index).setProperty(ids::mute, mute, undo);
}

void LiveSet::setReturnSolo(int index, bool solo)
{
    returnTrack(index).setProperty(ids::solo, solo, undo);
}

void LiveSet::setReturnPreFader(int index, bool pre)
{
    returnTrack(index).setProperty(ids::preFader, pre, undo);
}

double LiveSet::sendLevel(int trackIndex, int returnIndex) const
{
    const auto id = static_cast<juce::int64>(returnId(returnIndex));
    for (const auto& s : track(trackIndex).getChildWithName(ids::sends))
        if (static_cast<juce::int64>(s[ids::returnId]) == id)
            return s[ids::levelDb];
    return minVolumeDb;
}

void LiveSet::setSendLevel(int trackIndex, int returnIndex, double db)
{
    if (trackIndex < 0 || trackIndex >= numTracks() || returnIndex < 0 || returnIndex >= numReturns())
        return;
    db = juce::jlimit(minVolumeDb, maxVolumeDb, db);
    auto t = track(trackIndex);
    auto sendsNode = t.getChildWithName(ids::sends);
    if (!sendsNode.isValid())
    {
        sendsNode = juce::ValueTree(ids::sends);
        t.appendChild(sendsNode, undo);
    }
    const auto id = static_cast<juce::int64>(returnId(returnIndex));
    for (auto s : sendsNode)
        if (static_cast<juce::int64>(s[ids::returnId]) == id)
        {
            s.setProperty(ids::levelDb, db, undo);
            return;
        }
    juce::ValueTree s(ids::send);
    s.setProperty(ids::returnId, id, nullptr);
    s.setProperty(ids::levelDb, db, nullptr);
    sendsNode.appendChild(s, undo);
}

// --- Devices -------------------------------------------------------------------------------------------

juce::ValueTree LiveSet::makeDevice(const juce::String& typeId)
{
    juce::ValueTree d(ids::device);
    d.setProperty(ids::id, static_cast<juce::int64>(allocateId()), nullptr);
    d.setProperty(ids::deviceType, typeId, nullptr);
    d.setProperty(ids::enabled, true, nullptr);
    d.appendChild(juce::ValueTree(ids::params), nullptr);
    d.appendChild(juce::ValueTree(ids::deviceState), nullptr);
    return d;
}

juce::ValueTree LiveSet::chainOf(const juce::ValueTree& owner)
{
    return owner.getChildWithName(ids::devices);
}

juce::ValueTree LiveSet::insertDevice(juce::ValueTree chain, int index, const juce::String& typeId)
{
    jassert(chain.hasType(ids::devices));
    auto d = makeDevice(typeId);
    if (index < 0 || index > chain.getNumChildren())
        index = chain.getNumChildren();
    chain.addChild(d, index, undo);
    return d;
}

juce::ValueTree LiveSet::insertDeviceCopy(juce::ValueTree chain, int index, const juce::ValueTree& device)
{
    jassert(chain.hasType(ids::devices) && device.hasType(ids::device));
    auto copy = device.createCopy();
    reassignIds(copy);
    if (!copy.getChildWithName(ids::params).isValid())
        copy.appendChild(juce::ValueTree(ids::params), nullptr);
    if (!copy.getChildWithName(ids::deviceState).isValid())
        copy.appendChild(juce::ValueTree(ids::deviceState), nullptr);
    if (!copy.hasProperty(ids::enabled))
        copy.setProperty(ids::enabled, true, nullptr);
    if (index < 0 || index > chain.getNumChildren())
        index = chain.getNumChildren();
    chain.addChild(copy, index, undo);
    return copy;
}

void LiveSet::removeDevice(const juce::ValueTree& device)
{
    auto parent = device.getParent();
    if (parent.isValid())
        parent.removeChild(device, undo);
}

void LiveSet::moveDevice(const juce::ValueTree& device, juce::ValueTree toChain, int toIndex)
{
    auto from = device.getParent();
    if (!from.isValid() || !toChain.hasType(ids::devices))
        return;
    if (from == toChain)
    {
        const int current = from.indexOf(device);
        toIndex = juce::jlimit(0, from.getNumChildren() - 1, toIndex);
        if (current != toIndex)
            from.moveChild(current, toIndex, undo);
        return;
    }
    // The caller's reference keeps the node alive while it is out of the tree.
    from.removeChild(device, undo);
    if (toIndex < 0 || toIndex > toChain.getNumChildren())
        toIndex = toChain.getNumChildren();
    toChain.addChild(device, toIndex, undo);
}

void LiveSet::setDeviceEnabled(juce::ValueTree device, bool on)
{
    device.setProperty(ids::enabled, on, undo);
}

void LiveSet::setDeviceParameter(juce::ValueTree device, const juce::String& parameterId, double value)
{
    auto values = device.getChildWithName(ids::params);
    if (!values.isValid())
    {
        values = juce::ValueTree(ids::params);
        device.appendChild(values, undo);
    }
    values.setProperty(parameterId, value, undo);
}

double LiveSet::deviceParameter(const juce::ValueTree& device, const juce::String& parameterId, double fallback)
{
    const auto values = device.getChildWithName(ids::params);
    const auto* v = values.getPropertyPointer(parameterId);
    return v != nullptr ? static_cast<double>(*v) : fallback;
}

namespace
{
juce::ValueTree findDevice(const juce::ValueTree& tree, juce::int64 id)
{
    if (tree.hasType(ids::device) && static_cast<juce::int64>(tree[ids::id]) == id)
        return tree;
    if (tree.hasType(ids::slots) || tree.hasType(ids::scenes) || tree.hasType(ids::params))
        return {};
    for (const auto& child : tree)
        if (auto found = findDevice(child, id); found.isValid())
            return found;
    return {};
}
} // namespace

juce::ValueTree LiveSet::deviceById(std::int64_t id) const
{
    return findDevice(root, static_cast<juce::int64>(id));
}

juce::ValueTree LiveSet::chainOwnerOf(const juce::ValueTree& node)
{
    for (auto n = node; n.isValid(); n = n.getParent())
        if (n.hasType(ids::track) || n.hasType(ids::master) || n.hasType(ids::returnTrack))
            return n;
    return {};
}

std::int64_t LiveSet::allocateId()
{
    const juce::int64 next = root[ids::nextId];
    root.setProperty(ids::nextId, next + 1, nullptr);
    return next;
}

juce::ValueTree LiveSet::makeSlot() const
{
    juce::ValueTree s(ids::slot);
    s.setProperty(ids::hasStopButton, true, nullptr);
    return s;
}

juce::ValueTree LiveSet::makeTrack(int number)
{
    juce::ValueTree t(ids::track);
    t.setProperty(ids::id, static_cast<juce::int64>(allocateId()), nullptr);
    t.setProperty(ids::name, juce::String(number) + " MIDI", nullptr);
    t.setProperty(ids::colour, colourVar(number - 1), nullptr);
    t.setProperty(ids::volumeDb, 0.0, nullptr);
    t.setProperty(ids::pan, 0.0, nullptr);
    t.setProperty(ids::mute, false, nullptr);
    t.setProperty(ids::solo, false, nullptr);
    t.setProperty(ids::arm, false, nullptr);
    t.setProperty(ids::tuningId, juce::String(), nullptr);
    t.setProperty(ids::noteOffset, 0, nullptr);
    t.setProperty(ids::highNotePolicy, 0, nullptr);
    juce::ValueTree slotsNode(ids::slots);
    for (int s = 0; s < numScenes(); ++s)
        slotsNode.appendChild(makeSlot(), nullptr);
    t.appendChild(slotsNode, nullptr);
    juce::ValueTree chain(ids::devices);
    if (defaultInstrumentType().isNotEmpty())
        chain.appendChild(makeDevice(defaultInstrumentType()), nullptr);
    t.appendChild(chain, nullptr);
    return t;
}

juce::ValueTree LiveSet::makeScene()
{
    juce::ValueTree s(ids::scene);
    s.setProperty(ids::id, static_cast<juce::int64>(allocateId()), nullptr);
    s.setProperty(ids::name, juce::String(), nullptr);
    s.setProperty(ids::colour, static_cast<juce::int64>(0), nullptr);
    s.setProperty(ids::sceneTempo, 0.0, nullptr);
    s.setProperty(ids::sceneTimeSigNum, 0, nullptr);
    s.setProperty(ids::sceneTimeSigDen, 0, nullptr);
    return s;
}

void LiveSet::reassignIds(const juce::ValueTree& tree)
{
    // New IDs for everything that has one; macro mappings then follow their devices' new IDs.
    std::map<juce::int64, juce::int64> renamed;
    std::function<void(juce::ValueTree)> assign = [&](juce::ValueTree node) {
        if (node.hasType(ids::track) || node.hasType(ids::clip) || node.hasType(ids::scene) ||
            node.hasType(ids::device) || node.hasType(ids::rackChain) || node.hasType(ids::returnTrack))
        {
            const auto fresh = static_cast<juce::int64>(allocateId());
            if (node.hasType(ids::device))
                renamed[static_cast<juce::int64>(node[ids::id])] = fresh;
            node.setProperty(ids::id, fresh, nullptr);
        }
        for (auto child : node)
            assign(child);
    };
    assign(tree);
    std::function<void(juce::ValueTree)> remap = [&](juce::ValueTree node) {
        if (node.hasType(ids::mapping))
            if (const auto it = renamed.find(static_cast<juce::int64>(node[ids::targetDevice])); it != renamed.end())
                node.setProperty(ids::targetDevice, it->second, nullptr);
        for (auto child : node)
            remap(child);
    };
    remap(tree);
}

juce::int64 LiveSet::groupAt(int insertIndex) const
{
    // Right after a group's own track: inside it. Otherwise the group of the track that follows.
    if (insertIndex > 0 && isGroupTrack(insertIndex - 1))
        return static_cast<juce::int64>(trackId(insertIndex - 1));
    if (insertIndex >= 0 && insertIndex < numTracks())
        return static_cast<juce::int64>(track(insertIndex).getProperty(ids::groupId, static_cast<juce::int64>(0)));
    return 0;
}

int LiveSet::addTrack(int insertIndex)
{
    const int count = numTracks();
    if (insertIndex < 0 || insertIndex > count)
        insertIndex = count;
    auto t = makeTrack(count + 1);
    if (const auto g = groupAt(insertIndex); g != 0)
        t.setProperty(ids::groupId, g, nullptr);
    tracksNode().addChild(t, insertIndex, undo);
    return insertIndex;
}

void LiveSet::deleteTrack(int index)
{
    if (index < 0 || index >= numTracks())
        return;
    const int end = blockEnd(index);
    bool plainLeft = false;
    for (int t = 0; t < numTracks() && !plainLeft; ++t)
        plainLeft = (t < index || t >= end) && !isGroupTrack(t);
    if (!plainLeft)
        return; // a set keeps a track that can hold clips
    for (int t = end - 1; t >= index; --t)
        tracksNode().removeChild(t, undo);
}

int LiveSet::duplicateTrack(int index)
{
    if (index < 0 || index >= numTracks())
        return -1;
    return duplicateTracks(index, blockEnd(index) - 1);
}

int LiveSet::duplicateTracks(int first, int last)
{
    const int count = numTracks();
    if (first < 0 || last >= count || first > last)
        return -1;
    const auto parent = track(first).getProperty(ids::groupId, static_cast<juce::int64>(0));
    for (int t = first; t <= last; t = blockEnd(t))
        if (blockEnd(t) > last + 1 || track(t).getProperty(ids::groupId, static_cast<juce::int64>(0)) != parent)
            return -1;
    const int index = first;
    const int end = last + 1;
    // Copy the whole block, under new IDs, with the copies' members pointing at the copied groups.
    std::vector<juce::ValueTree> copies;
    std::map<juce::int64, juce::int64> renamed;
    for (int t = index; t < end; ++t)
    {
        auto copy = track(t).createCopy();
        const auto oldId = static_cast<juce::int64>(trackId(t));
        reassignIds(copy);
        renamed[oldId] = static_cast<juce::int64>(copy[ids::id]);
        copy.setProperty(ids::arm, false, nullptr);
        copy.setProperty(ids::solo, false, nullptr);
        copies.push_back(copy);
    }
    if (blockEnd(index) == end) // one track (or one group): its copy says so
        copies.front().setProperty(ids::name, track(index)[ids::name].toString() + " copy", nullptr);
    for (auto& copy : copies)
        if (const auto it = renamed.find(static_cast<juce::int64>(copy.getProperty(ids::groupId, static_cast<juce::int64>(0))));
            it != renamed.end())
            copy.setProperty(ids::groupId, it->second, nullptr);
    for (std::size_t i = 0; i < copies.size(); ++i)
        tracksNode().addChild(copies[i], end + static_cast<int>(i), undo);
    return end;
}

void LiveSet::moveTrack(int from, int to)
{
    const int count = numTracks();
    if (from < 0 || from >= count || to < 0 || to >= count || from == to)
        return;
    const int end = blockEnd(from);
    if (to >= from && to < end)
        return; // into itself
    // Where the block goes: before track `to` when moving up, after it when moving down.
    const int size = end - from;
    int before = to < from ? to : to + 1;
    if (to > from)
        before = std::min(count, before); // counted with the block still in place
    std::vector<juce::ValueTree> block;
    for (int t = from; t < end; ++t)
        block.push_back(track(t));
    for (int t = end - 1; t >= from; --t)
        tracksNode().removeChild(t, undo);
    if (before > from)
        before -= size;
    // With the block out, the group a track inserted here would join.
    const auto landed = groupAt(before);
    for (int i = 0; i < size; ++i)
        tracksNode().addChild(block[static_cast<std::size_t>(i)], before + i, undo);
    if (landed != static_cast<juce::int64>(block.front().getProperty(ids::groupId, static_cast<juce::int64>(0))))
        block.front().setProperty(ids::groupId, landed, undo);
}

bool LiveSet::isGroup(const juce::ValueTree& t)
{
    return t[ids::trackKind].toString() == "group";
}

bool LiveSet::isGroupTrack(int index) const
{
    return isGroup(track(index));
}

int LiveSet::parentGroupOf(int index) const
{
    const auto g = static_cast<juce::int64>(track(index).getProperty(ids::groupId, static_cast<juce::int64>(0)));
    if (g == 0)
        return -1;
    for (int t = index - 1; t >= 0; --t)
        if (trackId(t) == g)
            return t;
    return -1;
}

int LiveSet::groupDepth(int index) const
{
    int depth = 0;
    for (int g = parentGroupOf(index); g >= 0; g = parentGroupOf(g))
        ++depth;
    return depth;
}

int LiveSet::blockEnd(int index) const
{
    const int count = numTracks();
    if (index < 0 || index >= count)
        return index + 1;
    if (!isGroupTrack(index))
        return index + 1;
    // Everything after it that's deeper than it is inside it (members follow their group).
    const int depth = groupDepth(index);
    int end = index + 1;
    while (end < count && groupDepth(end) > depth)
        ++end;
    return end;
}

std::vector<int> LiveSet::groupMembers(int groupIndex) const
{
    std::vector<int> out;
    if (groupIndex < 0 || groupIndex >= numTracks() || !isGroupTrack(groupIndex))
        return out;
    const int end = blockEnd(groupIndex);
    for (int t = groupIndex + 1; t < end; t = blockEnd(t))
        out.push_back(t);
    return out;
}

std::vector<int> LiveSet::groupLeaves(int groupIndex) const
{
    std::vector<int> out;
    if (groupIndex < 0 || groupIndex >= numTracks() || !isGroupTrack(groupIndex))
        return out;
    for (int t = groupIndex + 1; t < blockEnd(groupIndex); ++t)
        if (!isGroupTrack(t))
            out.push_back(t);
    return out;
}

int LiveSet::groupTracks(int first, int last)
{
    const int count = numTracks();
    if (first < 0 || last >= count || first > last)
        return -1;
    // Whole blocks at one level: every block starting in the range ends inside it, and they all
    // share the first track's group.
    const auto parent = track(first).getProperty(ids::groupId, static_cast<juce::int64>(0));
    for (int t = first; t <= last; t = blockEnd(t))
        if (blockEnd(t) > last + 1 || track(t).getProperty(ids::groupId, static_cast<juce::int64>(0)) != parent)
            return -1;

    juce::ValueTree g(ids::track);
    const auto id = static_cast<juce::int64>(allocateId());
    g.setProperty(ids::id, id, nullptr);
    g.setProperty(ids::trackKind, "group", nullptr);
    g.setProperty(ids::name, "Group", nullptr);
    g.setProperty(ids::colour, track(first)[ids::colour], nullptr);
    g.setProperty(ids::volumeDb, 0.0, nullptr);
    g.setProperty(ids::pan, 0.0, nullptr);
    g.setProperty(ids::mute, false, nullptr);
    g.setProperty(ids::solo, false, nullptr);
    g.setProperty(ids::arm, false, nullptr);
    if (static_cast<juce::int64>(parent) != 0)
        g.setProperty(ids::groupId, parent, nullptr);
    juce::ValueTree slotsNode(ids::slots);
    for (int s = 0; s < numScenes(); ++s)
        slotsNode.appendChild(makeSlot(), nullptr);
    g.appendChild(slotsNode, nullptr);
    g.appendChild(juce::ValueTree(ids::devices), nullptr);

    std::vector<juce::ValueTree> tops;
    for (int t = first; t <= last; t = blockEnd(t))
        tops.push_back(track(t));
    tracksNode().addChild(g, first, undo);
    for (auto& t : tops)
        t.setProperty(ids::groupId, id, undo);
    return first;
}

void LiveSet::ungroup(int groupIndex)
{
    if (groupIndex < 0 || groupIndex >= numTracks() || !isGroupTrack(groupIndex))
        return;
    const auto parent = track(groupIndex).getProperty(ids::groupId, static_cast<juce::int64>(0));
    for (const int m : groupMembers(groupIndex))
        track(m).setProperty(ids::groupId, parent, undo);
    tracksNode().removeChild(groupIndex, undo);
}

void LiveSet::setGroupFolded(int groupIndex, bool fold)
{
    if (groupIndex >= 0 && groupIndex < numTracks() && isGroupTrack(groupIndex))
        track(groupIndex).setProperty(ids::folded, fold, nullptr);
}

void LiveSet::setTrackName(int index, const juce::String& newName)
{
    track(index).setProperty(ids::name, newName, undo);
}

void LiveSet::setTrackVolumeDb(int index, double db)
{
    track(index).setProperty(ids::volumeDb, juce::jlimit(minVolumeDb, maxVolumeDb, db), undo);
}

void LiveSet::setTrackPan(int index, double panValue)
{
    track(index).setProperty(ids::pan, juce::jlimit(-1.0, 1.0, panValue), undo);
}

void LiveSet::setTrackMute(int index, bool shouldMute)
{
    track(index).setProperty(ids::mute, shouldMute, undo);
}

void LiveSet::setExclusiveFlag(int index, const juce::Identifier& flag, bool on, bool additive)
{
    if (index < 0 || index >= numTracks())
        return;
    if (on && !additive)
        for (int i = 0; i < numTracks(); ++i)
            if (i != index && static_cast<bool>(track(i)[flag]))
                track(i).setProperty(flag, false, undo);
    track(index).setProperty(flag, on, undo);
}

void LiveSet::setTrackSolo(int index, bool shouldSolo, bool additive)
{
    setExclusiveFlag(index, ids::solo, shouldSolo, additive);
}

void LiveSet::setTrackArm(int index, bool shouldArm, bool additive)
{
    if (shouldArm && isGroupTrack(index))
        return; // nothing to play: a group has no instrument
    setExclusiveFlag(index, ids::arm, shouldArm, additive);
}

void LiveSet::setTrackTuning(int index, const juce::String& tuning)
{
    track(index).setProperty(ids::tuningId, tuning, undo);
}

void LiveSet::setTrackNoteOffset(int index, int offset)
{
    track(index).setProperty(ids::noteOffset, juce::jlimit(-127, 127, offset), undo);
}

void LiveSet::setTrackMidiInput(int index, const juce::String& input)
{
    track(index).setProperty(ids::midiInput, input, undo);
}

void LiveSet::setTrackMidiChannel(int index, int channel)
{
    track(index).setProperty(ids::midiChannel, juce::jlimit(0, 16, channel), undo);
}

void LiveSet::setTrackMonitoring(int index, Monitoring m)
{
    track(index).setProperty(ids::monitoring, static_cast<int>(m), undo);
}

void LiveSet::setTrackHighNotePolicy(int index, int policy)
{
    track(index).setProperty(ids::highNotePolicy, juce::jlimit(0, 1, policy), undo);
}

juce::ValueTree LiveSet::master() const
{
    return root.getChildWithName(ids::master);
}

void LiveSet::setMasterVolumeDb(double db)
{
    master().setProperty(ids::volumeDb, juce::jlimit(minVolumeDb, maxVolumeDb, db), undo);
}

void LiveSet::setMasterPan(double panValue)
{
    master().setProperty(ids::pan, juce::jlimit(-1.0, 1.0, panValue), undo);
}

// --- Scenes ---------------------------------------------------------------------------------------------

int LiveSet::numScenes() const
{
    return scenesNode().getNumChildren();
}

juce::ValueTree LiveSet::scene(int index) const
{
    return scenesNode().getChild(index);
}

int LiveSet::addScene(int insertIndex)
{
    const int count = numScenes();
    if (insertIndex < 0 || insertIndex > count)
        insertIndex = count;
    scenesNode().addChild(makeScene(), insertIndex, undo);
    for (auto t : tracksNode())
        t.getChildWithName(ids::slots).addChild(makeSlot(), insertIndex, undo);
    return insertIndex;
}

void LiveSet::deleteScene(int index)
{
    if (index < 0 || index >= numScenes())
        return;
    scenesNode().removeChild(index, undo);
    for (auto t : tracksNode())
        t.getChildWithName(ids::slots).removeChild(index, undo);
}

int LiveSet::duplicateScene(int index)
{
    if (index < 0 || index >= numScenes())
        return -1;
    auto sceneCopy = scene(index).createCopy();
    reassignIds(sceneCopy);
    scenesNode().addChild(sceneCopy, index + 1, undo);
    for (auto t : tracksNode())
    {
        auto slotsNode = t.getChildWithName(ids::slots);
        auto slotCopy = slotsNode.getChild(index).createCopy();
        reassignIds(slotCopy);
        slotsNode.addChild(slotCopy, index + 1, undo);
    }
    return index + 1;
}

void LiveSet::moveScene(int from, int to)
{
    const int count = numScenes();
    if (from < 0 || from >= count || to < 0 || to >= count || from == to)
        return;
    scenesNode().moveChild(from, to, undo);
    for (auto t : tracksNode())
        t.getChildWithName(ids::slots).moveChild(from, to, undo);
}

void LiveSet::setSceneName(int index, const juce::String& newName)
{
    scene(index).setProperty(ids::name, newName, undo);
}

void LiveSet::setSceneTempo(int index, double bpm)
{
    scene(index).setProperty(ids::sceneTempo, bpm <= 0.0 ? 0.0 : juce::jlimit(minTempo, maxTempo, bpm), undo);
}

void LiveSet::setSceneTimeSignature(int index, int numerator, int denominator)
{
    if (numerator == 0)
    {
        scene(index).setProperty(ids::sceneTimeSigNum, 0, undo);
        scene(index).setProperty(ids::sceneTimeSigDen, 0, undo);
        return;
    }
    if (numerator < 1 || numerator > 99 || !isValidDenominator(denominator))
        return;
    scene(index).setProperty(ids::sceneTimeSigNum, numerator, undo);
    scene(index).setProperty(ids::sceneTimeSigDen, denominator, undo);
}

// --- Slots and clips ------------------------------------------------------------------------------------

juce::ValueTree LiveSet::slot(int trackIndex, int sceneIndex) const
{
    return track(trackIndex).getChildWithName(ids::slots).getChild(sceneIndex);
}

juce::ValueTree LiveSet::clip(int trackIndex, int sceneIndex) const
{
    return slot(trackIndex, sceneIndex).getChildWithName(ids::clip);
}

bool LiveSet::hasStopButton(int trackIndex, int sceneIndex) const
{
    return slot(trackIndex, sceneIndex)[ids::hasStopButton];
}

void LiveSet::setHasStopButton(int trackIndex, int sceneIndex, bool has)
{
    slot(trackIndex, sceneIndex).setProperty(ids::hasStopButton, has, undo);
}

juce::ValueTree LiveSet::createClip(int trackIndex, int sceneIndex, double lengthBeats,
                                    const juce::String& clipName, bool undoable)
{
    auto s = slot(trackIndex, sceneIndex);
    if (!s.isValid() || isGroupTrack(trackIndex))
        return {}; // a group track has no clips of its own

    lengthBeats = std::max(lengthBeats, minLoopLength);
    juce::ValueTree c(ids::clip);
    c.setProperty(ids::id, static_cast<juce::int64>(allocateId()), nullptr);
    c.setProperty(ids::name, clipName, nullptr);
    c.setProperty(ids::colour, track(trackIndex)[ids::colour], nullptr);
    c.setProperty(ids::startMarker, 0.0, nullptr);
    c.setProperty(ids::loopStart, 0.0, nullptr);
    c.setProperty(ids::loopEnd, lengthBeats, nullptr);
    c.setProperty(ids::looping, true, nullptr);
    c.setProperty(ids::launchMode, static_cast<int>(LaunchMode::Trigger), nullptr);
    c.setProperty(ids::quantization, static_cast<int>(Quantization::Global), nullptr);
    c.setProperty(ids::legato, false, nullptr);
    c.setProperty(ids::followA, static_cast<int>(FollowAction::None), nullptr);
    c.setProperty(ids::followB, static_cast<int>(FollowAction::None), nullptr);
    c.setProperty(ids::followChanceA, 100.0, nullptr);
    c.setProperty(ids::followTime, beatsPerBar(), nullptr);
    c.appendChild(juce::ValueTree(ids::notes), nullptr);

    if (auto old = s.getChildWithName(ids::clip); old.isValid())
        s.removeChild(old, um(undoable));
    s.appendChild(c, um(undoable));
    return c;
}

void LiveSet::deleteClip(int trackIndex, int sceneIndex, bool undoable)
{
    auto s = slot(trackIndex, sceneIndex);
    if (auto c = s.getChildWithName(ids::clip); c.isValid())
        s.removeChild(c, um(undoable));
}

int LiveSet::duplicateClip(int trackIndex, int sceneIndex)
{
    const auto original = clip(trackIndex, sceneIndex);
    if (!original.isValid())
        return -1;
    for (int s = sceneIndex + 1; s < numScenes(); ++s)
    {
        if (clip(trackIndex, s).isValid())
            continue;
        auto copy = original.createCopy();
        reassignIds(copy);
        slot(trackIndex, s).appendChild(copy, undo);
        return s;
    }
    return -1;
}

void LiveSet::moveClip(int fromTrack, int fromScene, int toTrack, int toScene)
{
    if (fromTrack == toTrack && fromScene == toScene)
        return;
    if (toTrack < 0 || toTrack >= numTracks() || isGroupTrack(toTrack))
        return; // a group track has no clips of its own
    auto c = clip(fromTrack, fromScene);
    auto destination = slot(toTrack, toScene);
    if (!c.isValid() || !destination.isValid())
        return;
    if (auto old = destination.getChildWithName(ids::clip); old.isValid())
        destination.removeChild(old, undo);
    slot(fromTrack, fromScene).removeChild(c, undo);
    destination.appendChild(c, undo);
}

void LiveSet::moveClips(const std::vector<ClipMove>& moves)
{
    // Lift every source first, then put each one down: overlapping moves can't overwrite a clip
    // that hasn't moved yet.
    std::vector<std::pair<juce::ValueTree, ClipMove>> lifted;
    for (const auto& m : moves)
    {
        if (m.toTrack < 0 || m.toTrack >= numTracks() || m.toScene < 0 || m.toScene >= numScenes() || isGroupTrack(m.toTrack))
            continue;
        auto c = clip(m.fromTrack, m.fromScene);
        if (!c.isValid())
            continue;
        slot(m.fromTrack, m.fromScene).removeChild(c, undo);
        lifted.emplace_back(c, m);
    }
    for (auto& [c, m] : lifted)
    {
        auto destination = slot(m.toTrack, m.toScene);
        if (auto old = destination.getChildWithName(ids::clip); old.isValid())
            destination.removeChild(old, undo);
        destination.appendChild(c, undo);
    }
}

juce::ValueTree LiveSet::putClipCopy(int trackIndex, int sceneIndex, const juce::ValueTree& source)
{
    auto destination = slot(trackIndex, sceneIndex);
    if (!destination.isValid() || !source.isValid() || isGroupTrack(trackIndex))
        return {};
    auto copy = source.createCopy();
    reassignIds(copy);
    if (auto old = destination.getChildWithName(ids::clip); old.isValid())
        destination.removeChild(old, undo);
    destination.appendChild(copy, undo);
    return copy;
}

int LiveSet::paletteSize() noexcept
{
    return static_cast<int>(palette.size());
}

juce::uint32 LiveSet::paletteColour(int index) noexcept
{
    return palette[static_cast<std::size_t>(juce::jlimit(0, paletteSize() - 1, index))];
}

void LiveSet::setColour(juce::ValueTree node, juce::uint32 argb)
{
    if (node.isValid())
        node.setProperty(ids::colour, static_cast<juce::int64>(argb), undo);
}

std::pair<int, int> LiveSet::findClip(std::int64_t clipId) const
{
    for (int t = 0; t < numTracks(); ++t)
        for (int s = 0; s < numScenes(); ++s)
            if (const auto c = clip(t, s); c.isValid() && idOf(c) == clipId)
                return {t, s};
    return {-1, -1};
}

juce::ValueTree LiveSet::clipById(std::int64_t clipId) const
{
    const auto [t, s] = findClip(clipId);
    return t >= 0 ? clip(t, s) : juce::ValueTree();
}

void LiveSet::setClipProperty(juce::ValueTree c, const juce::Identifier& property, const juce::var& value,
                              bool undoable)
{
    c.setProperty(property, value, um(undoable));
}

void LiveSet::setClipLoop(juce::ValueTree c, double newLoopStart, double newLoopEnd, bool undoable)
{
    newLoopStart = std::max(0.0, newLoopStart);
    newLoopEnd = std::max(newLoopEnd, newLoopStart + minLoopLength);
    c.setProperty(ids::loopStart, newLoopStart, um(undoable));
    c.setProperty(ids::loopEnd, newLoopEnd, um(undoable));
}

std::pair<int, int> LiveSet::clipTimeSignature(const juce::ValueTree& c) const
{
    const int num = c.getProperty(ids::clipTimeSigNum, 0);
    const int den = c.getProperty(ids::clipTimeSigDen, 0);
    if (num > 0 && den > 0)
        return {num, den};
    return {timeSigNumerator(), timeSigDenominator()};
}

void LiveSet::setClipTimeSignature(juce::ValueTree c, int numerator, int denominator)
{
    if (numerator <= 0)
    {
        c.removeProperty(ids::clipTimeSigNum, undo);
        c.removeProperty(ids::clipTimeSigDen, undo);
        return;
    }
    if (numerator > 99 || !isValidDenominator(denominator)) // the set's limits
        return;
    c.setProperty(ids::clipTimeSigNum, numerator, undo);
    c.setProperty(ids::clipTimeSigDen, denominator, undo);
}

std::vector<int> LiveSet::highlightOf(const juce::ValueTree& node)
{
    std::vector<int> out;
    juce::StringArray tokens;
    tokens.addTokens(node[ids::highlight].toString(), " ", {});
    for (const auto& t : tokens)
        if (t.isNotEmpty() && t.containsOnly("0123456789") && t.getIntValue() < 128)
            out.push_back(t.getIntValue());
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

std::vector<int> LiveSet::effectiveHighlight(const juce::ValueTree& c)
{
    if (c.hasProperty(ids::highlight))
        return highlightOf(c);
    return highlightOf(c.getParent().getParent().getParent()); // CLIP -> SLOT -> SLOTS -> TRACK
}

void LiveSet::setHighlight(juce::ValueTree node, std::vector<int> indices)
{
    std::sort(indices.begin(), indices.end());
    indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
    juce::StringArray tokens;
    for (const int i : indices)
        if (i >= 0 && i < 128)
            tokens.add(juce::String(i));
    if (tokens.isEmpty())
        node.removeProperty(ids::highlight, undo);
    else
        node.setProperty(ids::highlight, tokens.joinIntoString(" "), undo);
}

// --- Notes ------------------------------------------------------------------------------------------------

juce::ValueTree LiveSet::addNote(const juce::ValueTree& c, const NoteData& data, bool undoable)
{
    auto notesNode = c.getChildWithName(ids::notes);
    if (!notesNode.isValid())
        return {};
    juce::ValueTree n(ids::note);
    n.setProperty(ids::pitch, juce::jlimit(0, 127, data.pitch), nullptr);
    n.setProperty(ids::start, std::max(0.0, data.start), nullptr);
    n.setProperty(ids::length, std::max(1.0e-6, data.length), nullptr);
    n.setProperty(ids::velocity, juce::jlimit(1, 127, data.velocity), nullptr);
    n.setProperty(ids::muted, data.muted, nullptr);
    notesNode.appendChild(n, um(undoable));
    return n;
}

void LiveSet::removeNote(const juce::ValueTree& c, const juce::ValueTree& n, bool undoable)
{
    c.getChildWithName(ids::notes).removeChild(n, um(undoable));
}

void LiveSet::setNote(juce::ValueTree n, const NoteData& data)
{
    n.setProperty(ids::pitch, juce::jlimit(0, 127, data.pitch), undo);
    n.setProperty(ids::start, std::max(0.0, data.start), undo);
    n.setProperty(ids::length, std::max(1.0e-6, data.length), undo);
    n.setProperty(ids::velocity, juce::jlimit(1, 127, data.velocity), undo);
    n.setProperty(ids::muted, data.muted, undo);
}

NoteData LiveSet::noteData(const juce::ValueTree& n)
{
    NoteData d;
    d.pitch = n[ids::pitch];
    d.start = n[ids::start];
    d.length = n[ids::length];
    d.velocity = n[ids::velocity];
    d.muted = n[ids::muted];
    return d;
}

std::vector<NoteData> LiveSet::notesOf(const juce::ValueTree& c)
{
    std::vector<NoteData> result;
    const auto notesNode = c.getChildWithName(ids::notes);
    result.reserve(static_cast<std::size_t>(notesNode.getNumChildren()));
    for (const auto& n : notesNode)
        result.push_back(noteData(n));
    return result;
}

} // namespace lattice
