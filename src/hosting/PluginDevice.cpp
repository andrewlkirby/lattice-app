#include "hosting/PluginDevice.h"

#include "hosting/clap/ClapInstance.h"

#include <cmath>
#include <limits>

namespace lattice::hosting
{

namespace
{
constexpr float unsent = std::numeric_limits<float>::quiet_NaN();
/** How long after a state is restored the plugin's announcements are its restore, not changes:
    some formats announce restored values later, from the message loop. */
constexpr juce::uint32 settleMs = 750;

/** A parameter's group, as the plugin groups it: "Osc / Osc 1". Empty: none. */
juce::String groupOf(const juce::AudioProcessorParameterGroup& root, const juce::AudioProcessorParameter* p)
{
    juce::StringArray path;
    for (const auto* g : root.getGroupsForParameter(const_cast<juce::AudioProcessorParameter*>(p)))
        if (g->getName().isNotEmpty())
            path.add(g->getName());
    return path.joinIntoString(" / ");
}

float toNormalised(const ParameterSpec& spec, float plain) noexcept
{
    return spec.maxValue > spec.minValue ? juce::jlimit(0.0f, 1.0f, (plain - spec.minValue) / (spec.maxValue - spec.minValue)) : 0.0f;
}

float toPlain(const ParameterSpec& spec, float normalised) noexcept
{
    return spec.constrain(spec.minValue + juce::jlimit(0.0f, 1.0f, normalised) * (spec.maxValue - spec.minValue));
}
} // namespace

juce::String parameterIdFor(const juce::AudioProcessorParameter& parameter)
{
    juce::String raw;
    if (const auto* hosted = dynamic_cast<const juce::HostedAudioProcessorParameter*>(&parameter))
        raw = hosted->getParameterID();
    if (raw.isEmpty())
        raw = juce::String(parameter.getParameterIndex());
    juce::String id = "p";
    for (const auto c : raw)
        id << (juce::CharacterFunctions::isLetterOrDigit(c) || c == '_' || c == '.' ? juce::String::charToString(c) : "_");
    return id;
}

std::vector<ParameterSpec> describeParameters(juce::AudioPluginInstance& plugin, const std::shared_ptr<PluginText>& text)
{
    std::vector<ParameterSpec> specs;
    const auto& parameters = plugin.getParameters();
    specs.reserve(static_cast<std::size_t>(parameters.size()));
    juce::StringArray used;
    const std::weak_ptr<PluginText> weak = text;
    for (int i = 0; i < parameters.size(); ++i)
    {
        const auto* p = parameters[i];
        auto id = parameterIdFor(*p);
        if (used.contains(id))
            id << "_" << i; // two that read the same once made valid
        used.add(id);
        auto name = p->getName(64).trim();
        if (name.isEmpty())
            name = "Parameter " + juce::String(i + 1);

        const int steps = p->getNumSteps();
        const bool discrete = p->isDiscrete() && steps >= 2 && steps < 0x7fffffff;
        ParameterSpec s;
        if (p->isBoolean())
            s = ParameterSpec::toggle(id, name, p->getDefaultValue() >= 0.5f);
        else if (discrete && steps <= 64)
        {
            auto choices = p->getAllValueStrings();
            if (choices.size() != steps)
            {
                choices.clear();
                for (int v = 0; v < steps; ++v)
                    choices.add(p->getText(static_cast<float>(v) / static_cast<float>(steps - 1), 64));
            }
            s = ParameterSpec::choice(id, name, choices, juce::roundToInt(p->getDefaultValue() * static_cast<float>(steps - 1)));
        }
        else
        {
            s = ParameterSpec::continuous(id, name, 0.0f, 1.0f, p->getDefaultValue());
            if (discrete)
                s.interval = 1.0f / static_cast<float>(steps - 1);
        }
        s.group = groupOf(plugin.getParameterTree(), p);
        s.modulatable = p->isAutomatable();
        const auto range = std::make_pair(s.minValue, s.maxValue);
        s.formatter = [weak, i, range](float plain) -> juce::String {
            const float n = range.second > range.first ? (plain - range.first) / (range.second - range.first) : 0.0f;
            if (const auto t = weak.lock(); t != nullptr && t->plugin != nullptr && i < t->plugin->getParameters().size())
                return t->plugin->getParameters()[i]->getText(juce::jlimit(0.0f, 1.0f, n), 64);
            return juce::String(juce::roundToInt(n * 100.0f)) + " %";
        };
        s.parser = [weak, i, range](const juce::String& typed) -> std::optional<float> {
            if (const auto t = weak.lock(); t != nullptr && t->plugin != nullptr && i < t->plugin->getParameters().size())
                return range.first + t->plugin->getParameters()[i]->getValueForText(typed) * (range.second - range.first);
            return std::nullopt;
        };
        specs.push_back(std::move(s));
    }
    return specs;
}

// --- HostedPluginDevice -------------------------------------------------------------------------------

namespace
{
std::vector<ParameterSpec> specsWith(juce::AudioPluginInstance& plugin, const std::shared_ptr<PluginText>& text)
{
    text->plugin = &plugin;
    return describeParameters(plugin, text);
}
} // namespace

HostedPluginDevice::HostedPluginDevice(std::int64_t deviceId, const juce::String& typeId,
                                       std::unique_ptr<juce::AudioPluginInstance> plugin)
    : HostedPluginDevice(deviceId, typeId, std::move(plugin), std::make_shared<PluginText>())
{
}

HostedPluginDevice::HostedPluginDevice(std::int64_t deviceId, const juce::String& typeId,
                                       std::unique_ptr<juce::AudioPluginInstance> plugin, std::shared_ptr<PluginText> text)
    : StudioDevice(typeId, plugin->getName(), pluginTypes::kindOf(typeId), specsWith(*plugin, text)),
      id(deviceId),
      instance(std::move(plugin)),
      words(std::move(text)),
      count(instance->getParameters().size())
{
    JUCE_ASSERT_MESSAGE_THREAD
    for (auto* p : instance->getParameters())
        pluginParameters.push_back(p);
    const auto n = static_cast<std::size_t>(count);
    watched = std::make_unique<std::atomic<bool>[]>(n);
    watchList.assign(n, 0);
    everWatched.assign(n, false);
    sent = std::make_unique<std::atomic<float>[]>(n);
    reported = std::make_unique<std::atomic<float>[]>(n);
    reportFlags = std::make_unique<std::atomic<int>[]>(n);
    known.resize(n);
    for (std::size_t i = 0; i < n; ++i)
    {
        sent[i].store(unsent);
        known[i] = pluginParameters[i]->getValue();
    }
    instance->setPlayHead(this);
    instance->addListener(this);
}

HostedPluginDevice::~HostedPluginDevice()
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (onGoing)
        onGoing();
    instance->removeListener(this);
    instance->setPlayHead(nullptr);
    words->plugin = nullptr;
    if (keepPlugin)
        keepPlugin(std::move(instance));
    else
        instance->releaseResources();
}

void HostedPluginDevice::setMpe(bool on) noexcept
{
    mpeOn.store(on, std::memory_order_relaxed);
    if (auto* clap = dynamic_cast<ClapPluginInstance*>(instance.get()))
        clap->setMpeInput(on);
}

juce::MemoryBlock HostedPluginDevice::saveState()
{
    JUCE_ASSERT_MESSAGE_THREAD
    juce::MemoryBlock state;
    instance->getStateInformation(state);
    return state;
}

void HostedPluginDevice::restoreState(const juce::MemoryBlock& state)
{
    JUCE_ASSERT_MESSAGE_THREAD
    {
        const juce::ScopedValueSetter<bool> inside(restoring, true);
        if (state.getSize() > 0)
            instance->setStateInformation(state.getData(), static_cast<int>(state.getSize()));
    }
    for (int i = 0; i < count; ++i)
    {
        known[static_cast<std::size_t>(i)] = pluginParameters[static_cast<std::size_t>(i)]->getValue();
        sent[static_cast<std::size_t>(i)].store(unsent); // the document's values are sent again: they win
    }
    settleUntil = juce::Time::getMillisecondCounter() + settleMs;
    stateDirty = false;
}

void HostedPluginDevice::stateChanged(const juce::ValueTree& state)
{
    // Its own state is the host's to restore (once, when the device is made); MPE follows STATE.
    const int wanted = state.getProperty(pluginTypes::mpe, -1);
    setMpe(wanted < 0 ? mpeDefault : wanted == 1);
}

int HostedPluginDevice::latencyReserve() const noexcept
{
    return static_cast<int>(sampleRate() * 0.5);
}

float HostedPluginDevice::knownValue(int index) const noexcept
{
    return index >= 0 && index < count ? toPlain(parameter(index).spec(), known[static_cast<std::size_t>(index)]) : 0.0f;
}

void HostedPluginDevice::watch(int index)
{
    const auto i = static_cast<std::size_t>(index);
    if (!everWatched[i])
    {
        everWatched[i] = true;
        const int n = watchCount.load(std::memory_order_relaxed);
        watchList[static_cast<std::size_t>(n)] = index;
        watchCount.store(n + 1, std::memory_order_release);
    }
    watched[i].store(true, std::memory_order_release);
}

void HostedPluginDevice::unwatch(int index)
{
    watched[static_cast<std::size_t>(index)].store(false, std::memory_order_release);
}

void HostedPluginDevice::applyDocumentValue(int index, const juce::var* documentValue)
{
    if (documentValue == nullptr)
    {
        unwatch(index); // its value is the plugin's own again
        return;
    }
    parameter(index).setBase(static_cast<float>(static_cast<double>(*documentValue)));
    watch(index);
}

void HostedPluginDevice::adoptPluginValue(int index, float value) noexcept
{
    if (index < 0 || index >= count)
        return;
    const float plain = parameter(index).spec().constrain(value);
    sent[static_cast<std::size_t>(index)].store(plain);
    parameter(index).setBase(plain);
}

void HostedPluginDevice::prepare(double newSampleRate, int maxBlockSize)
{
    instance->releaseResources();
    instance->setRateAndBufferSizeDetails(newSampleRate, maxBlockSize);
    instance->prepareToPlay(newSampleRate, maxBlockSize);
    setLatency(instance->getLatencySamples());
    latencyDirty = false;

    const auto channelsOf = [this](bool input, int bus) {
        std::vector<int> channels;
        if (bus < instance->getBusCount(input) && instance->getBus(input, bus)->isEnabled())
            for (int c = 0; c < instance->getBus(input, bus)->getNumberOfChannels(); ++c)
                channels.push_back(instance->getChannelIndexInProcessBlockBuffer(input, bus, c));
        return channels;
    };
    mainIn = kind() == DeviceKind::AudioEffect ? channelsOf(true, 0) : std::vector<int>();
    sideIn = channelsOf(true, 1);
    mainOut = channelsOf(false, 0);
    const int channels = juce::jmax(2, instance->getTotalNumInputChannels(), instance->getTotalNumOutputChannels());
    buffer.setSize(channels, maxBlockSize, false, true, false);
    midi.ensureSize(16384);

    // The document's values, before the first block.
    const int watching = watchCount.load(std::memory_order_acquire);
    for (int k = 0; k < watching; ++k)
    {
        const int i = watchList[static_cast<std::size_t>(k)];
        if (!watched[static_cast<std::size_t>(i)].load())
            continue;
        const float plain = parameter(i).plain();
        pluginParameters[static_cast<std::size_t>(i)]->setValue(toNormalised(parameter(i).spec(), plain));
        sent[static_cast<std::size_t>(i)].store(plain);
    }
}

void HostedPluginDevice::process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept
{
    const int n = io.numSamples;
    blockTime = time;

    // Document values that changed (an edit, undo, a macro), to the plugin.
    const int watching = watchCount.load(std::memory_order_acquire);
    for (int k = 0; k < watching; ++k)
    {
        const auto i = static_cast<std::size_t>(watchList[static_cast<std::size_t>(k)]);
        if (!watched[i].load(std::memory_order_acquire))
            continue;
        const auto& p = parameter(static_cast<int>(i));
        const float plain = p.plain();
        if (plain == sent[i].load(std::memory_order_relaxed))
            continue;
        sent[i].store(plain, std::memory_order_relaxed);
        pluginParameters[i]->setValue(toNormalised(p.spec(), plain));
    }

    // MIDI in.
    midi.clear();
    if (notesOff.exchange(false, std::memory_order_relaxed))
        for (int ch = 0; ch < 16; ++ch)
        {
            const std::uint8_t sustainOff[3]{static_cast<std::uint8_t>(0xb0 | ch), 64, 0};
            const std::uint8_t allOff[3]{static_cast<std::uint8_t>(0xb0 | ch), 123, 0};
            midi.addEvent(sustainOff, 3, 0);
            midi.addEvent(allOff, 3, 0);
        }
    if (io.midiIn != nullptr)
    {
        const bool perNote = mpeOn.load(std::memory_order_relaxed);
        for (const auto& e : *io.midiIn)
        {
            if (e.size == 0 || e.size > 3)
                continue;
            if (!perNote && (e.flags & TimedMidi::perNote) != 0)
            {
                const int type = e.bytes[0] & 0xf0;
                if (type != 0x90 && type != 0x80)
                    continue; // a member channel's own bend, pressure or timbre: not for a plugin without MPE
                std::uint8_t bytes[3]{static_cast<std::uint8_t>(type), e.bytes[1], e.bytes[2]};
                midi.addEvent(bytes, 3, juce::jlimit(0, n - 1, e.sampleOffset));
                continue;
            }
            midi.addEvent(e.bytes, e.size, juce::jlimit(0, n - 1, e.sampleOffset));
        }
    }

    // Audio in: the chain's signal to its main input, the sidechain to its second.
    buffer.setSize(buffer.getNumChannels(), n, false, false, true);
    for (int c = 0; c < buffer.getNumChannels(); ++c)
        juce::FloatVectorOperations::clear(buffer.getWritePointer(c), n);
    if (kind() == DeviceKind::AudioEffect)
        for (std::size_t k = 0; k < mainIn.size(); ++k)
            juce::FloatVectorOperations::copy(buffer.getWritePointer(mainIn[k]),
                                              io.channels[std::min(static_cast<int>(k), io.numChannels - 1)], n);
    if (io.sidechain != nullptr)
        for (std::size_t k = 0; k < sideIn.size(); ++k)
            juce::FloatVectorOperations::copy(buffer.getWritePointer(sideIn[k]), io.sidechain[k % 2], n);

    {
        const rt::ScopedForeignCode foreign;
        const auto before = rt::foreignAllocationsOnThisThread();
        instance->processBlock(buffer, midi);
        if (const auto made = rt::foreignAllocationsOnThisThread() - before; made > 0)
            allocations.fetch_add(made, std::memory_order_relaxed);
    }

    // Audio out (a MIDI effect leaves the chain's audio as it is).
    if (kind() != DeviceKind::MidiEffect)
        for (int c = 0; c < io.numChannels; ++c)
        {
            if (mainOut.empty())
                juce::FloatVectorOperations::clear(io.channels[c], n);
            else
                juce::FloatVectorOperations::copy(io.channels[c],
                                                  buffer.getReadPointer(mainOut[std::min(static_cast<std::size_t>(c), mainOut.size() - 1)]), n);
        }

    // MIDI out, to the next device.
    if (kind() == DeviceKind::MidiEffect && io.midiOut != nullptr)
        for (const auto meta : midi)
        {
            if (meta.numBytes < 1 || meta.numBytes > 3)
                continue;
            TimedMidi e;
            e.sampleOffset = juce::jlimit(0, n - 1, meta.samplePosition);
            e.size = static_cast<std::uint8_t>(meta.numBytes);
            std::copy_n(meta.data, meta.numBytes, e.bytes);
            io.midiOut->add(e);
        }
}

juce::Optional<juce::AudioPlayHead::PositionInfo> HostedPluginDevice::getPosition() const
{
    PositionInfo info;
    const auto& t = blockTime;
    info.setBpm(t.tempo);
    info.setTimeSignature(juce::AudioPlayHead::TimeSignature{t.timeSigNumerator, t.timeSigDenominator});
    info.setTimeInSamples(t.blockStartSample);
    info.setTimeInSeconds(t.sampleRate > 0.0 ? static_cast<double>(t.blockStartSample) / t.sampleRate : 0.0);
    info.setPpqPosition(t.beatAtStart);
    info.setPpqPositionOfLastBarStart(t.barStartBeat);
    info.setBarCount(t.barIndex);
    info.setIsPlaying(t.playing);
    return info;
}

// --- What the plugin did ----------------------------------------------------------------------------------

void HostedPluginDevice::audioProcessorParameterChanged(juce::AudioProcessor*, int parameterIndex, float newValue)
{
    if (parameterIndex < 0 || parameterIndex >= count)
        return;
    const auto i = static_cast<std::size_t>(parameterIndex);
    reported[i].store(newValue, std::memory_order_relaxed);
    reportFlags[i].fetch_or(1, std::memory_order_release);
    anyReport.store(true, std::memory_order_release);
}

void HostedPluginDevice::audioProcessorParameterChangeGestureBegin(juce::AudioProcessor*, int parameterIndex)
{
    if (parameterIndex < 0 || parameterIndex >= count)
        return;
    reportFlags[static_cast<std::size_t>(parameterIndex)].fetch_or(2, std::memory_order_release);
    anyReport.store(true, std::memory_order_release);
}

void HostedPluginDevice::audioProcessorParameterChangeGestureEnd(juce::AudioProcessor*, int parameterIndex)
{
    if (parameterIndex < 0 || parameterIndex >= count)
        return;
    reportFlags[static_cast<std::size_t>(parameterIndex)].fetch_or(4, std::memory_order_release);
    anyReport.store(true, std::memory_order_release);
}

void HostedPluginDevice::audioProcessorChanged(juce::AudioProcessor*, const juce::AudioProcessorListener::ChangeDetails& details)
{
    if (details.latencyChanged)
        latencyDirty.store(true);
    if (details.parameterInfoChanged)
        infoDirty.store(true);
    if (details.nonParameterStateChanged || details.programChanged)
        stateDirty.store(true);
}

void HostedPluginDevice::takeReports(std::vector<Report>& out)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (!anyReport.exchange(false, std::memory_order_acq_rel))
        return;
    const bool settling = juce::Time::getMillisecondCounter() < settleUntil;
    for (int i = 0; i < count; ++i)
    {
        const auto k = static_cast<std::size_t>(i);
        const int flags = reportFlags[k].exchange(0, std::memory_order_acq_rel);
        if (flags == 0)
            continue;
        const float v = reported[k].load(std::memory_order_relaxed);
        const bool changed = (flags & 1) != 0 && std::abs(v - known[k]) > 1.0e-6f;
        if ((flags & 1) != 0)
            known[k] = v;
        if (settling)
            continue; // its restore, announced late
        if (changed)
            stateDirty.store(true);
        if (!changed && (flags & 6) == 0)
            continue;
        Report r;
        r.index = i;
        r.value = toPlain(parameter(i).spec(), v);
        r.changed = changed;
        r.begin = (flags & 2) != 0;
        r.end = (flags & 4) != 0;
        out.push_back(r);
    }
    if (settling)
        stateDirty.store(false);
}

bool HostedPluginDevice::takeLatencyChange(int& newLatency)
{
    if (!latencyDirty.exchange(false))
        return false;
    newLatency = instance->getLatencySamples();
    return newLatency != latencySamples();
}

} // namespace lattice::hosting
