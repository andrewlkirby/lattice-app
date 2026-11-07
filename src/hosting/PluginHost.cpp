#include "hosting/PluginHost.h"

#include "core/model/UndoNames.h"
#include "engine/SessionController.h"
#include "hosting/PluginFormats.h"
#include "hosting/PluginLibrary.h"
#include "hosting/PluginWindows.h"
#include "hosting/clap/ClapInstance.h"
#include "hosting/clap/ClapLibrary.h"

namespace lattice::hosting
{

namespace
{
/** How long a plugin's state must rest before it's written into the document. */
constexpr juce::uint32 captureAfterMs = 1000;

int kindIndex(const juce::String& typeId)
{
    return typeId == pluginTypes::instrument ? 0 : typeId == pluginTypes::audioEffect ? 1 : 2;
}

juce::ValueTree stateOf(juce::ValueTree device)
{
    return device.getOrCreateChildWithName(ids::deviceState, nullptr);
}

/** Whether something else refers to a device's parameter: a rack's macro, or an address MIDI Map
    or Key Map keeps ("device:<id>/<parameter>"). */
bool isMapped(const juce::ValueTree& root, std::int64_t deviceId, const juce::String& parameterId)
{
    const auto address = "device:" + juce::String(deviceId) + "/" + parameterId;
    std::function<bool(const juce::ValueTree&)> search = [&](const juce::ValueTree& node) {
        if (node.hasType(ids::mapping) && static_cast<juce::int64>(node[ids::targetDevice]) == deviceId &&
            node[ids::parameter].toString() == parameterId)
            return true;
        for (int i = 0; i < node.getNumProperties(); ++i)
            if (const auto& v = node.getProperty(node.getPropertyName(i)); v.isString() && v.toString() == address)
                return true;
        for (const auto& child : node)
            if (!child.hasType(ids::notes) && search(child))
                return true;
        return false;
    };
    return search(root);
}
} // namespace

PluginHost::PluginHost(SessionController& s, PluginLibrary* l) : session(s), library(l)
{
    addHostedFormats(formatManager);
    pluginWindows = std::make_unique<PluginWindows>();
    for (const auto* type : {pluginTypes::instrument, pluginTypes::audioEffect, pluginTypes::midiEffect})
        noParameters[kindIndex(type)] = std::make_unique<PluginSpecDevice>(type, "Plugin", std::vector<ParameterSpec>{});
    session.snapshots().setDeviceSource(this);
    session.set().onFlushDeviceState = [this] { captureAll(); };
    if (library != nullptr)
        library->addChangeListener(this);
}

PluginHost::~PluginHost()
{
    stopTimer();
    if (library != nullptr)
        library->removeChangeListener(this);
    pluginWindows->closeAll();
    // The engine frees its devices later: they mustn't call back.
    for (auto& [id, l] : live)
    {
        l.device->onGoing = nullptr;
        l.device->keepPlugin = nullptr;
        if (auto* clap = dynamic_cast<ClapPluginInstance*>(&l.device->plugin()))
        {
            clap->onRestartRequested = nullptr;
            clap->onEditorClosed = nullptr;
        }
    }
    session.set().onFlushDeviceState = nullptr;
    session.snapshots().setDeviceSource(nullptr);
}

// --- Putting plugins on tracks -------------------------------------------------------------------------------

juce::String PluginHost::typeFor(const scan::ScannedPlugin& plugin)
{
    const auto& d = plugin.description;
    if (d.isInstrument || d.category.containsIgnoreCase("Instrument"))
        return pluginTypes::instrument;
    // No audio, notes in and out: a note effect.
    if (d.numOutputChannels == 0 && (plugin.midiOutputs > 0 || d.category.containsIgnoreCase("Note Effect")))
        return pluginTypes::midiEffect;
    return pluginTypes::audioEffect;
}

void PluginHost::describeDevice(LiveSet& set, juce::ValueTree device, const scan::ScannedPlugin& plugin)
{
    auto* undo = set.undoManager();
    device.setProperty(ids::name, plugin.description.name, undo);
    auto state = device.getChildWithName(ids::deviceState);
    if (!state.isValid())
    {
        state = juce::ValueTree(ids::deviceState);
        device.appendChild(state, undo);
    }
    if (const auto old = state.getChildWithName(pluginTypes::description); old.isValid())
        state.removeChild(old, undo);
    if (const auto xml = plugin.toXml())
        state.appendChild(juce::ValueTree::fromXml(*xml), undo);
}

std::optional<scan::ScannedPlugin> PluginHost::pluginOf(const juce::ValueTree& device)
{
    if (!pluginTypes::isPlugin(device[ids::deviceType].toString()))
        return std::nullopt;
    const auto node = device.getChildWithName(ids::deviceState).getChildWithName(pluginTypes::description);
    if (!node.isValid())
        return std::nullopt;
    const auto xml = node.createXml();
    return xml != nullptr ? scan::ScannedPlugin::fromXml(*xml) : std::nullopt;
}

// --- DeviceSource ---------------------------------------------------------------------------------------------

std::unique_ptr<StudioDevice> PluginHost::create(const juce::ValueTree& node, const DeviceCreateContext&)
{
    JUCE_ASSERT_MESSAGE_THREAD
    const auto type = node[ids::deviceType].toString();
    const auto id = LiveSet::idOf(node);
    const auto plugin = pluginOf(node);
    if (!plugin)
        return std::make_unique<PluginStandIn>(type, PluginStandIn::Why::missing, "This device doesn't say which plugin it is");
    const auto identity = identityOf(plugin->description);

    // The same device on another chain still (a move): its state as it is now, for the new one.
    if (const auto it = live.find(id); it != live.end())
        capture(id, it->second);

    if (const auto it = ready.find(id); it != ready.end() && it->second.identity == identity)
    {
        auto entry = std::move(it->second);
        ready.erase(it);
        auto device = std::make_unique<HostedPluginDevice>(id, type, std::move(entry.instance));
        auto* raw = device.get();
        if (!entry.live)
        {
            const auto data = node.getChildWithName(ids::deviceState)[pluginTypes::data].toString();
            juce::MemoryBlock state;
            if (data.isNotEmpty() && state.fromBase64Encoding(data))
                raw->restoreState(state);
        }
        raw->setMpeDefault(plugin->mpe == 1);
        raw->onGoing = [this, id, raw] {
            const auto at = live.find(id);
            if (at == live.end() || at->second.device != raw)
                return; // a newer device has this ID now
            pluginWindows->close(id);
            capture(id, at->second); // into its node, even if it's out of the set now (undo keeps it)
            live.erase(at);
            sendChangeMessage();
        };
        if (auto* clap = dynamic_cast<ClapPluginInstance*>(&raw->plugin()))
        {
            clap->onRestartRequested = [this, id] { restart(id); };
            clap->onEditorClosed = [this, id] { pluginWindows->close(id); };
        }
        Live l;
        l.device = raw;
        l.node = node;
        live[id] = std::move(l);
        failures.erase(id);
        if (!node.getChildWithName(ids::deviceState).hasProperty(pluginTypes::configured))
            newDevices.insert(id);
        wakeUp();
        sendChangeMessage();
        return device;
    }

    if (const auto it = failures.find(id); it != failures.end() && it->second.identity == identity)
        return std::make_unique<PluginStandIn>(type, it->second.why, it->second.message);

    if (!restarting.contains(id) && !waiting.contains(id))
    {
        waiting[id] = {identity, plugin->description};
        queue.push_back(id);
        wakeUp();
    }
    return std::make_unique<PluginStandIn>(type, PluginStandIn::Why::loading);
}

bool PluginHost::isStale(const StudioDevice& device, const juce::ValueTree& node) const
{
    const auto id = LiveSet::idOf(node);
    if (const auto* standIn = dynamic_cast<const PluginStandIn*>(&device))
    {
        if (ready.contains(id))
            return true;
        if (standIn->why() == PluginStandIn::Why::loading)
            return failures.contains(id) || (!waiting.contains(id) && !restarting.contains(id));
        return !failures.contains(id); // it failed before: tried again
    }
    if (const auto* hosted = dynamic_cast<const HostedPluginDevice*>(&device))
        return restarting.contains(hosted->deviceId());
    return false;
}

// --- Loading ----------------------------------------------------------------------------------------------------

std::optional<juce::PluginDescription> PluginHost::installed(const juce::PluginDescription& saved) const
{
    if (library != nullptr)
        if (const auto found = library->find(saved))
            return found->description;
    // No library (tests, tools), or it hasn't seen it: where it was saved from, if it's there.
    if (juce::File::isAbsolutePath(saved.fileOrIdentifier) && juce::File(saved.fileOrIdentifier).exists())
        return saved;
    return std::nullopt;
}

void PluginHost::prepareBuses(juce::AudioPluginInstance& instance, const juce::String& typeId) const
{
    // Stereo in (an effect) and out, a stereo sidechain if it has one, and nothing else (extra
    // outputs aren't routed anywhere yet: they'd cost without being heard).
    const auto plain = instance.getBusesLayout();
    auto wanted = plain;
    for (int i = 0; i < wanted.inputBuses.size(); ++i)
        wanted.inputBuses.getReference(i) = (i == 0 && typeId == pluginTypes::audioEffect) || i == 1
                                                ? juce::AudioChannelSet::stereo()
                                                : juce::AudioChannelSet::disabled();
    for (int i = 0; i < wanted.outputBuses.size(); ++i)
        wanted.outputBuses.getReference(i) = i == 0 && typeId != pluginTypes::midiEffect ? juce::AudioChannelSet::stereo()
                                                                                          : juce::AudioChannelSet::disabled();
    if (instance.setBusesLayout(wanted))
        return;
    if (wanted.inputBuses.size() > 1)
    {
        wanted.inputBuses.getReference(1) = juce::AudioChannelSet::disabled(); // without the sidechain, then
        if (instance.setBusesLayout(wanted))
            return;
    }
    instance.setBusesLayout(plain); // as the plugin has it
}

void PluginHost::loadNext()
{
    while (!queue.empty())
    {
        const auto id = queue.front();
        queue.pop_front();
        const auto it = waiting.find(id);
        if (it == waiting.end())
            continue;
        const auto request = std::move(it->second);
        waiting.erase(it);
        const auto node = session.set().deviceById(id);
        if (!node.isValid())
            continue; // gone while it waited

        const auto type = node[ids::deviceType].toString();
        if (const auto found = installed(request.description))
        {
            juce::String error;
            std::unique_ptr<juce::AudioPluginInstance> instance;
            auto& engine = session.audioEngine();
            withoutErrorBoxes([&] {
                instance = formatManager.createPluginInstance(*found, engine.deviceSampleRate(), engine.deviceBlockSize(), error);
            });
            if (instance != nullptr)
            {
                prepareBuses(*instance, type);
                ready[id] = {request.identity, std::move(instance), false};
            }
            else
                failures[id] = {request.identity, PluginStandIn::Why::failed,
                                request.description.name + " wouldn't load" + (error.isNotEmpty() ? ": " + error : juce::String())};
        }
        else
            failures[id] = {request.identity, PluginStandIn::Why::missing,
                            request.description.name + " (" + request.description.pluginFormatName + ") isn't installed"};
        session.snapshots().refreshDevice(id);
        sendChangeMessage();
        return; // one a turn: the window stays responsive while a set's plugins load
    }
}

void PluginHost::finishLoading()
{
    for (int guard = 0; guard < 1000 && (!queue.empty() || !ready.empty()); ++guard)
    {
        loadNext();
        session.flush();
        session.snapshots().collectGarbage();
    }
    idle();
    session.flush();
}

void PluginHost::restart(std::int64_t id)
{
    const auto it = live.find(id);
    if (it == live.end() || restarting.contains(id))
        return;
    const auto plugin = pluginOf(it->second.node);
    if (!plugin)
        return;
    restarting.insert(id);
    it->second.device->keepPlugin = [this, id, identity = identityOf(plugin->description)](std::unique_ptr<juce::AudioPluginInstance> instance) {
        ready[id] = {identity, std::move(instance), true};
        restarting.erase(id);
        session.snapshots().refreshDevice(id);
        wakeUp();
    };
    session.snapshots().refreshDevice(id);
}

void PluginHost::changeListenerCallback(juce::ChangeBroadcaster*)
{
    retryMissing();
}

void PluginHost::retryMissing()
{
    // A scan may have found what wasn't installed.
    std::vector<std::int64_t> retry;
    for (const auto& [id, f] : failures)
        if (f.why == PluginStandIn::Why::missing)
            retry.push_back(id);
    for (const auto id : retry)
    {
        failures.erase(id);
        session.snapshots().refreshDevice(id);
    }
}

void PluginHost::wakeUp()
{
    if (!isTimerRunning())
        startTimerHz(30);
}

// --- What plugins did, into the document ---------------------------------------------------------------------

void PluginHost::idle()
{
    JUCE_ASSERT_MESSAGE_THREAD
    loadNext();

    std::vector<std::int64_t> ids;
    for (const auto& [id, l] : live)
        ids.push_back(id);
    for (const auto id : ids)
        if (const auto it = live.find(id); it != live.end())
        {
            if (auto* clap = dynamic_cast<ClapPluginInstance*>(&it->second.device->plugin()))
                clap->idle(); // so tests without a message loop see what it did
            takeChanges(id, it->second);
        }

    for (const auto id : std::exchange(newDevices, {}))
        configureNew(id);

    const auto now = juce::Time::getMillisecondCounter();
    for (auto& [id, l] : live)
        if (l.changedAt != 0 && now - l.changedAt >= captureAfterMs)
            capture(id, l);

    if (queue.empty() && live.empty() && ready.empty() && restarting.empty())
        stopTimer();
}

void PluginHost::takeChanges(std::int64_t id, Live& l)
{
    auto* d = l.device;
    reports.clear();
    d->takeReports(reports);
    auto& set = session.set();
    auto* undo = set.undoManager();
    for (const auto& r : reports)
    {
        const auto& spec = d->parameter(r.index).spec();
        if (r.begin)
            l.gestures.insert(r.index);
        bool inDocument = l.node.getChildWithName(ids::params).hasProperty(spec.id);
        if (!inDocument && configuring.contains(id) && (r.changed || r.begin))
        {
            addToPages(l.node, spec.id); // touched in its window while configuring
            inDocument = true;
        }
        if (inDocument && r.changed && l.node.getParent().isValid())
        {
            const auto name = undoNames::make(undoNames::withValue(spec.name, spec.format(r.value)),
                                              undoNames::locationOf(l.node, session.deviceRegistry(), spec.group));
            // A gesture's changes are one step, renamed as the value moves; the rest a step each.
            if (l.gestures.contains(r.index) && !r.begin && l.gestureSteps.contains(r.index))
                undo->setCurrentTransactionName(name);
            else
            {
                undo->beginNewTransaction(name);
                if (l.gestures.contains(r.index))
                    l.gestureSteps.insert(r.index);
            }
            d->adoptPluginValue(r.index, r.value);
            set.setDeviceParameter(l.node, spec.id, r.value);
        }
        if (r.end)
        {
            l.gestures.erase(r.index);
            l.gestureSteps.erase(r.index);
        }
    }

    if (d->takeStateChanged())
        l.changedAt = juce::jmax<juce::uint32>(1, juce::Time::getMillisecondCounter());
    if (int latency = 0; d->takeLatencyChange(latency))
    {
        if (d->followLatency(latency))
            session.snapshots().refreshDevice(id); // the chain is published again: delay compensation follows
        else
            restart(id); // more than its dry path holds: prepared again
    }
    if (d->takeParameterInfoChange())
    {
        ++revisions[id];
        sendChangeMessage();
    }
}

void PluginHost::capture(std::int64_t, Live& l)
{
    l.changedAt = 0;
    if (!l.node.isValid())
        return;
    const auto state = l.device->saveState();
    stateOf(l.node).setProperty(pluginTypes::data, state.toBase64Encoding(), nullptr); // not an edit of its own
}

void PluginHost::captureAll()
{
    for (auto& [id, l] : live)
        if (l.changedAt != 0)
            capture(id, l);
}

void PluginHost::configureNew(std::int64_t id)
{
    const auto it = live.find(id);
    if (it == live.end())
        return;
    auto& l = it->second;
    auto* d = l.device;
    auto node = l.node;
    if (!node.isValid() || node.getChildWithName(ids::deviceState).hasProperty(pluginTypes::configured))
        return;

    // Its plugin's saved default, or the first parameters it lets a host automate.
    juce::StringArray pages;
    if (const auto plugin = pluginOf(node); plugin && loadDefaultPages)
        for (const auto& p : juce::StringArray::fromTokens(loadDefaultPages(identityOf(plugin->description)), " ", {}))
            if (d->parameterIndex(p) >= 0 && !pages.contains(p))
                pages.add(p);
    if (pages.isEmpty())
        for (int i = 0; i < d->numParameters() && pages.size() < defaultPageParameters; ++i)
            if (d->parameter(i).spec().modulatable)
                pages.add(d->parameter(i).id());

    // Part of putting it there, not an edit of its own: no undo step.
    setPages(node, pages, nullptr);
    auto values = node.getOrCreateChildWithName(ids::params, nullptr);
    for (const auto& p : pages)
        if (!values.hasProperty(p))
        {
            const int index = d->parameterIndex(p);
            const float v = d->knownValue(index);
            d->adoptPluginValue(index, v);
            values.setProperty(p, v, nullptr);
        }
    l.changedAt = juce::jmax<juce::uint32>(1, juce::Time::getMillisecondCounter()); // its first state, soon
    sendChangeMessage();
}

// --- What the UI sees --------------------------------------------------------------------------------------------

HostedPluginDevice* PluginHost::liveDevice(std::int64_t deviceId) const
{
    const auto it = live.find(deviceId);
    return it != live.end() ? it->second.device : nullptr;
}

PluginStatus PluginHost::statusOf(const juce::ValueTree& device) const
{
    if (!pluginTypes::isPlugin(device[ids::deviceType].toString()))
        return {};
    const auto id = LiveSet::idOf(device);
    if (live.contains(id))
        return {PluginStatus::State::ready, {}};
    if (const auto it = failures.find(id); it != failures.end())
        return {it->second.why == PluginStandIn::Why::missing ? PluginStatus::State::missing : PluginStatus::State::failed,
                it->second.message};
    return {PluginStatus::State::loading, {}};
}

const StudioDevice* PluginHost::describe(const juce::ValueTree& device)
{
    const auto type = device[ids::deviceType].toString();
    if (!pluginTypes::isPlugin(type))
        return nullptr;
    const auto id = LiveSet::idOf(device);
    const auto* d = liveDevice(id);
    if (d == nullptr)
        return noParameters[kindIndex(type)].get();
    const auto pages = device.getChildWithName(ids::deviceState)[pluginTypes::configured].toString();
    const int revision = revisions[id];
    auto& cached = described[id];
    if (cached.spec != nullptr && cached.device == d && cached.pages == pages && cached.revision == revision)
        return cached.spec;

    std::vector<ParameterSpec> chosen;
    for (const auto& p : juce::StringArray::fromTokens(pages, " ", {}))
        if (const int index = d->parameterIndex(p); index >= 0)
            chosen.push_back(d->parameter(index).spec());
    specs.push_back(std::make_unique<PluginSpecDevice>(type, d->getName(), std::move(chosen)));
    cached = {d, pages, revision, specs.back().get()};
    return cached.spec;
}

std::vector<ParameterSpec> PluginHost::allParameters(const juce::ValueTree& device)
{
    std::vector<ParameterSpec> all;
    if (const auto* d = liveDevice(LiveSet::idOf(device)))
        for (int i = 0; i < d->numParameters(); ++i)
            all.push_back(d->parameter(i).spec());
    return all;
}

void PluginHost::setConfiguring(std::int64_t deviceId, bool on)
{
    if (on ? configuring.insert(deviceId).second : configuring.erase(deviceId) > 0)
        sendChangeMessage();
}

juce::StringArray PluginHost::pagesOf(const juce::ValueTree& device) const
{
    return juce::StringArray::fromTokens(device.getChildWithName(ids::deviceState)[pluginTypes::configured].toString(), " ", {});
}

void PluginHost::setPages(juce::ValueTree device, const juce::StringArray& pages, juce::UndoManager* undo)
{
    stateOf(device).setProperty(pluginTypes::configured, pages.joinIntoString(" "), undo);
}

void PluginHost::addToPages(juce::ValueTree device, const juce::String& parameterId)
{
    auto* d = liveDevice(LiveSet::idOf(device));
    const int index = d != nullptr ? d->parameterIndex(parameterId) : -1;
    auto pages = pagesOf(device);
    if (index < 0 || pages.contains(parameterId))
        return;
    auto& set = session.set();
    session.beginEdit("Add " + d->parameter(index).spec().name + " to the device", device);
    pages.add(parameterId);
    setPages(device, pages, set.undoManager());
    if (!device.getChildWithName(ids::params).hasProperty(parameterId))
    {
        const float v = d->knownValue(index);
        d->adoptPluginValue(index, v);
        set.setDeviceParameter(device, parameterId, v);
    }
}

void PluginHost::removeFromPages(juce::ValueTree device, const juce::String& parameterId)
{
    auto pages = pagesOf(device);
    if (!pages.contains(parameterId))
        return;
    auto& set = session.set();
    const auto* d = liveDevice(LiveSet::idOf(device));
    const int index = d != nullptr ? d->parameterIndex(parameterId) : -1;
    session.beginEdit("Remove " + (index >= 0 ? d->parameter(index).spec().name : parameterId) + " from the device", device);
    pages.removeString(parameterId);
    setPages(device, pages, set.undoManager());
    // Its value is the plugin's own again, unless something maps it.
    if (auto values = device.getChildWithName(ids::params);
        values.hasProperty(parameterId) && !isMapped(set.state(), LiveSet::idOf(device), parameterId))
        values.removeProperty(parameterId, set.undoManager());
}

void PluginHost::saveDefaultPages(const juce::ValueTree& device)
{
    if (const auto plugin = pluginOf(device); plugin && storeDefaultPages)
        storeDefaultPages(identityOf(plugin->description), pagesOf(device).joinIntoString(" "));
}

bool PluginHost::mpeOf(const juce::ValueTree& device) const
{
    const int wanted = device.getChildWithName(ids::deviceState).getProperty(pluginTypes::mpe, -1);
    if (wanted >= 0)
        return wanted == 1;
    const auto plugin = pluginOf(device);
    return plugin && plugin->mpe == 1;
}

void PluginHost::setMpe(juce::ValueTree device, bool on)
{
    session.beginEdit(on ? "MPE on" : "MPE off", device);
    stateOf(device).setProperty(pluginTypes::mpe, on ? 1 : 0, session.set().undoManager());
}

} // namespace lattice::hosting
