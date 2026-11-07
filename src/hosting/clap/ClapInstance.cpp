#include "hosting/clap/ClapInstance.h"

#include "hosting/clap/ClapEditor.h"
#include "hosting/clap/ClapFormat.h"
#include "hosting/clap/ClapLibrary.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>
#include <variant>

namespace lattice::hosting
{

namespace
{
/** Set while the message thread makes a call CLAP reserves for the audio thread, at a moment the
    audio thread can't be in the plugin (stop_processing() before deactivate(), with the audio
    stopped): the plugin's thread check then agrees. */
thread_local bool actingAsAudioThread = false;

struct ActingAsAudioThread
{
    ActingAsAudioThread() noexcept { actingAsAudioThread = true; }
    ~ActingAsAudioThread() noexcept { actingAsAudioThread = false; }
};

juce::String textOf(const char* s)
{
    return s != nullptr ? juce::String(juce::CharPointer_UTF8(s)) : juce::String();
}

juce::AudioChannelSet channelSetFor(std::uint32_t channels)
{
    if (channels == 1)
        return juce::AudioChannelSet::mono();
    if (channels == 2)
        return juce::AudioChannelSet::stereo();
    return juce::AudioChannelSet::discreteChannels(static_cast<int>(channels));
}

#if JUCE_WINDOWS
constexpr const char* windowApi = CLAP_WINDOW_API_WIN32;
#elif JUCE_LINUX || JUCE_BSD
constexpr const char* windowApi = CLAP_WINDOW_API_X11;
#else
constexpr const char* windowApi = nullptr; // macOS windows come with the macOS port
#endif
} // namespace

// --- The host a plugin talks to ------------------------------------------------------------------------

/** What a plugin calls back: requests are flags, acted on in the instance's idle() on the message
    thread (a plugin may call most of them from any thread). */
struct ClapHostContext
{
    clap_host_t host{};
    ClapPluginInstance* owner = nullptr; // from the end of construction

    std::atomic<bool> restart{false}, callback{false}, flush{false}, latencyChanged{false}, stateDirty{false};
    std::atomic<std::uint32_t> rescanFlags{0};
    std::atomic<bool> resizeRequested{false}, closed{false};
    std::atomic<std::uint32_t> resizeWidth{0}, resizeHeight{0};

    static ClapHostContext& of(const clap_host_t* h) { return *static_cast<ClapHostContext*>(h->host_data); }

    ClapHostContext()
    {
        host.clap_version = CLAP_VERSION;
        host.host_data = this;
        host.name = "Lattice";
        host.vendor = "Lattice";
        host.url = "";
        host.version = "0.6";
        host.get_extension = getExtension;
        host.request_restart = [](const clap_host_t* h) { of(h).restart = true; };
        host.request_process = [](const clap_host_t*) {}; // it's processed every block
        host.request_callback = [](const clap_host_t* h) { of(h).callback = true; };
    }

    static const void* getExtension(const clap_host_t*, const char* id);
};

namespace
{
const clap_host_thread_check_t threadCheck{
    [](const clap_host_t*) { return juce::MessageManager::existsAndIsCurrentThread(); },
    [](const clap_host_t*) { return rt::isAudioThread() || actingAsAudioThread; }};

const clap_host_log_t hostLog{[](const clap_host_t*, clap_log_severity severity, const char* message) {
    juce::ignoreUnused(severity, message);
#if JUCE_DEBUG
    if (!rt::isAudioThread() && severity >= CLAP_LOG_WARNING)
        DBG("CLAP plugin: " << textOf(message));
#endif
}};

const clap_host_params_t hostParams{
    [](const clap_host_t* h, clap_param_rescan_flags flags) { ClapHostContext::of(h).rescanFlags.fetch_or(flags); },
    [](const clap_host_t*, clap_id, clap_param_clear_flags) {},
    [](const clap_host_t* h) { ClapHostContext::of(h).flush = true; }};

const clap_host_state_t hostState{[](const clap_host_t* h) { ClapHostContext::of(h).stateDirty = true; }};

const clap_host_latency_t hostLatency{[](const clap_host_t* h) { ClapHostContext::of(h).latencyChanged = true; }};

const clap_host_tail_t hostTail{[](const clap_host_t*) {}};

const clap_host_gui_t hostGui{
    [](const clap_host_t*) {},
    [](const clap_host_t* h, std::uint32_t width, std::uint32_t height) {
        auto& c = ClapHostContext::of(h);
        c.resizeWidth = width;
        c.resizeHeight = height;
        c.resizeRequested = true;
        return true;
    },
    [](const clap_host_t*) { return true; },
    [](const clap_host_t*) { return true; },
    [](const clap_host_t* h, bool) { ClapHostContext::of(h).closed = true; }};

const clap_host_audio_ports_t hostAudioPorts{[](const clap_host_t*, std::uint32_t) { return false; },
                                             [](const clap_host_t* h, std::uint32_t) { ClapHostContext::of(h).restart = true; }};

const clap_host_note_ports_t hostNotePorts{
    [](const clap_host_t*) -> std::uint32_t { return CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE; },
    [](const clap_host_t* h, std::uint32_t) { ClapHostContext::of(h).restart = true; }};
} // namespace

// Timers and fds act on the instance, which registers them (the message thread).
class ClapPluginInstance::PluginTimer final : public juce::Timer
{
public:
    PluginTimer(ClapPluginInstance& o, clap_id i) : owner(o), id(i) {}
    void timerCallback() override
    {
        if (owner.timers != nullptr)
            owner.timers->on_timer(owner.plugin, id);
    }

private:
    ClapPluginInstance& owner;
    clap_id id;
};

namespace
{
const clap_host_timer_support_t hostTimers{
    [](const clap_host_t* h, std::uint32_t periodMs, clap_id* id) {
        auto* owner = ClapHostContext::of(h).owner;
        if (owner == nullptr || id == nullptr || !juce::MessageManager::existsAndIsCurrentThread())
            return false;
        return owner->registerTimer(periodMs, *id);
    },
    [](const clap_host_t* h, clap_id id) {
        auto* owner = ClapHostContext::of(h).owner;
        return owner != nullptr && owner->unregisterTimer(id);
    }};

#if JUCE_LINUX || JUCE_BSD
const clap_host_posix_fd_support_t hostFds{
    [](const clap_host_t* h, int fd, clap_posix_fd_flags_t flags) {
        auto* owner = ClapHostContext::of(h).owner;
        return owner != nullptr && owner->watchFd(fd, flags);
    },
    [](const clap_host_t* h, int fd, clap_posix_fd_flags_t flags) {
        auto* owner = ClapHostContext::of(h).owner;
        return owner != nullptr && owner->unwatchFd(fd) && owner->watchFd(fd, flags);
    },
    [](const clap_host_t* h, int fd) {
        auto* owner = ClapHostContext::of(h).owner;
        return owner != nullptr && owner->unwatchFd(fd);
    }};
#endif
} // namespace

const void* ClapHostContext::getExtension(const clap_host_t*, const char* id)
{
    if (id == nullptr)
        return nullptr;
    const auto is = [id](const char* name) { return std::strcmp(id, name) == 0; };
    if (is(CLAP_EXT_THREAD_CHECK))
        return &threadCheck;
    if (is(CLAP_EXT_LOG))
        return &hostLog;
    if (is(CLAP_EXT_PARAMS))
        return &hostParams;
    if (is(CLAP_EXT_STATE))
        return &hostState;
    if (is(CLAP_EXT_LATENCY))
        return &hostLatency;
    if (is(CLAP_EXT_TAIL))
        return &hostTail;
    if (is(CLAP_EXT_GUI))
        return &hostGui;
    if (is(CLAP_EXT_TIMER_SUPPORT))
        return &hostTimers;
    if (is(CLAP_EXT_AUDIO_PORTS))
        return &hostAudioPorts;
    if (is(CLAP_EXT_NOTE_PORTS))
        return &hostNotePorts;
#if JUCE_LINUX || JUCE_BSD
    if (is(CLAP_EXT_POSIX_FD_SUPPORT))
        return &hostFds;
#endif
    return nullptr;
}

// --- Event lists -------------------------------------------------------------------------------------------

/** A list of events, in storage sized in prepare(): never allocates while processing. The same
    object is a CLAP input list (in) or output list (out). */
class ClapPluginInstance::Events
{
public:
    Events()
    {
        input.ctx = this;
        input.size = [](const clap_input_events_t* list) { return static_cast<const Events*>(list->ctx)->count; };
        input.get = [](const clap_input_events_t* list, std::uint32_t index) {
            return static_cast<const Events*>(list->ctx)->get(index);
        };
        output.ctx = this;
        output.try_push = [](const clap_output_events_t* list, const clap_event_header_t* e) {
            return static_cast<Events*>(list->ctx)->push(e);
        };
    }

    void prepare(std::size_t bytes, std::size_t maxEvents)
    {
        storage.assign((bytes + 7) / 8, 0);
        offsets.assign(maxEvents, 0);
        clear();
    }

    void clear() noexcept
    {
        used = 0;
        count = 0;
    }

    bool push(const clap_event_header_t* e) noexcept LATTICE_NONBLOCKING
    {
        if (e == nullptr || e->size < sizeof(clap_event_header_t))
            return false;
        const std::size_t words = (e->size + 7) / 8;
        if (count >= offsets.size() || used + words > storage.size())
            return false;
        std::memcpy(storage.data() + used, e, e->size);
        offsets[count++] = static_cast<std::uint32_t>(used);
        used += words;
        return true;
    }

    template <typename Event>
    bool add(const Event& e) noexcept LATTICE_NONBLOCKING
    {
        return push(&e.header);
    }

    std::uint32_t size() const noexcept { return count; }
    const clap_event_header_t* get(std::uint32_t index) const noexcept
    {
        return index < count ? reinterpret_cast<const clap_event_header_t*>(storage.data() + offsets[index]) : nullptr;
    }

    clap_input_events_t input{};
    clap_output_events_t output{};

private:
    std::vector<std::uint64_t> storage; // 8-byte aligned
    std::vector<std::uint32_t> offsets; // in words
    std::size_t used = 0;
    std::uint32_t count = 0;
};

// --- Parameters ------------------------------------------------------------------------------------------

class ClapParameter final : public juce::HostedAudioProcessorParameter
{
public:
    ClapParameter(ClapPluginInstance& o, const clap_param_info_t& i, double value) : owner(o)
    {
        setInfo(i);
        normalised.store(toNormalised(value));
    }

    void setInfo(const clap_param_info_t& i)
    {
        id = i.id;
        cookie = i.cookie;
        name = textOf(i.name);
        module = textOf(i.module);
        minValue = i.min_value;
        maxValue = i.max_value;
        defaultPlain = i.default_value;
        stepped = (i.flags & CLAP_PARAM_IS_STEPPED) != 0;
        automatable = (i.flags & CLAP_PARAM_IS_AUTOMATABLE) != 0 && (i.flags & (CLAP_PARAM_IS_HIDDEN | CLAP_PARAM_IS_READONLY)) == 0;
    }

    double toPlain(float n) const noexcept
    {
        const double p = minValue + static_cast<double>(juce::jlimit(0.0f, 1.0f, n)) * (maxValue - minValue);
        return stepped ? std::round(p) : p;
    }
    float toNormalised(double plain) const noexcept
    {
        return maxValue > minValue ? static_cast<float>(juce::jlimit(0.0, 1.0, (plain - minValue) / (maxValue - minValue))) : 0.0f;
    }

    /** The plugin changed it: no event back to it. */
    void setFromPlugin(double plain) noexcept { normalised.store(toNormalised(plain), std::memory_order_relaxed); }

    float getValue() const override { return normalised.load(std::memory_order_relaxed); }
    void setValue(float newValue) override
    {
        newValue = juce::jlimit(0.0f, 1.0f, newValue);
        normalised.store(newValue, std::memory_order_relaxed);
        owner.queueValue(index, toPlain(newValue));
    }
    float getDefaultValue() const override { return toNormalised(defaultPlain); }
    juce::String getName(int maximumLength) const override { return name.substring(0, maximumLength); }
    juce::String getLabel() const override { return {}; }
    int getNumSteps() const override
    {
        return stepped ? juce::jmax(2, static_cast<int>(std::lround(maxValue - minValue)) + 1)
                       : juce::AudioProcessor::getDefaultNumParameterSteps();
    }
    bool isDiscrete() const override { return stepped; }
    bool isBoolean() const override { return stepped && std::lround(maxValue - minValue) == 1; }
    juce::String getText(float value, int maximumLength) const override
    {
        return owner.valueToText(index, toPlain(value)).substring(0, maximumLength);
    }
    float getValueForText(const juce::String& text) const override
    {
        if (const auto plain = owner.textToValue(index, text))
            return toNormalised(*plain);
        return getValue();
    }
    bool isAutomatable() const override { return automatable; }
    juce::String getParameterID() const override { return juce::String(static_cast<juce::uint32>(id)); }

    ClapPluginInstance& owner;
    int index = 0; // in the instance's parameters
    clap_id id = CLAP_INVALID_ID;
    void* cookie = nullptr;
    juce::String name, module;
    double minValue = 0.0, maxValue = 1.0, defaultPlain = 0.0;
    bool stepped = false, automatable = true;

private:
    std::atomic<float> normalised{0.0f};
};

// --- Creating ------------------------------------------------------------------------------------------------

std::unique_ptr<ClapPluginInstance> ClapPluginInstance::create(const juce::PluginDescription& description, double sampleRate,
                                                               int blockSize, juce::String& error)
{
    JUCE_ASSERT_MESSAGE_THREAD
    auto library = ClapLibrary::open(juce::File(description.fileOrIdentifier), error);
    if (library == nullptr)
        return nullptr;
    const auto* factory = library->factory();
    const clap_plugin_descriptor_t* found = nullptr;
    for (std::uint32_t i = 0; i < factory->get_plugin_count(factory); ++i)
    {
        const auto* d = factory->get_plugin_descriptor(factory, i);
        if (d != nullptr && d->id != nullptr && ClapPluginFormat::uniqueIdOf(textOf(d->id)) == description.uniqueId)
            found = d;
    }
    if (found == nullptr)
    {
        error = "isn't in " + juce::File(description.fileOrIdentifier).getFileName() + " any more";
        return nullptr;
    }

    auto context = std::make_unique<ClapHostContext>();
    const clap_plugin_t* plugin = nullptr;
    bool initialised = false;
    withoutErrorBoxes([&] {
        plugin = factory->create_plugin(factory, &context->host, found->id);
        initialised = plugin != nullptr && plugin->init(plugin);
    });
    if (plugin == nullptr || !initialised)
    {
        if (plugin != nullptr)
            plugin->destroy(plugin);
        error = "failed to start";
        return nullptr;
    }

    // Its buses, from its audio ports.
    BusesProperties buses;
    if (const auto* ports = static_cast<const clap_plugin_audio_ports_t*>(plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS)))
        for (const bool isInput : {true, false})
            for (std::uint32_t i = 0; i < ports->count(plugin, isInput); ++i)
            {
                clap_audio_port_info_t port{};
                if (!ports->get(plugin, i, isInput, &port) || port.channel_count == 0)
                    continue;
                auto name = textOf(port.name);
                if (name.isEmpty())
                    name = (isInput ? "Input " : "Output ") + juce::String(i + 1);
                if (isInput)
                    buses.addBus(true, name, channelSetFor(port.channel_count), true);
                else
                    buses.addBus(false, name, channelSetFor(port.channel_count), true);
            }

    std::unique_ptr<ClapPluginInstance> instance(
        new ClapPluginInstance(std::move(buses), std::move(library), std::move(context), plugin, description));
    instance->setRateAndBufferSizeDetails(sampleRate, blockSize);
    return instance;
}

ClapPluginInstance::ClapPluginInstance(BusesProperties buses, std::shared_ptr<ClapLibrary> lib,
                                       std::unique_ptr<ClapHostContext> context, const clap_plugin_t* p,
                                       const juce::PluginDescription& description)
    : juce::AudioPluginInstance(buses), library(std::move(lib)), host(std::move(context)), plugin(p), info(description)
{
    host->owner = this;
    inEvents = std::make_unique<Events>();
    outEvents = std::make_unique<Events>();
    mainIn = std::make_unique<Events>();
    mainOut = std::make_unique<Events>();
    for (auto& channel : noteIds)
        channel.fill(-1);
    channelTimbre.fill(0.5f);
    readExtensions();
    readParameters();
    startTimerHz(30);
}

ClapPluginInstance::~ClapPluginInstance()
{
    JUCE_ASSERT_MESSAGE_THREAD
    stopTimer();
    jassert(editor == nullptr); // its window goes first (JUCE's rule for every plugin)
    deactivate();
    pluginTimers.clear();
#if JUCE_LINUX || JUCE_BSD
    for (const int fd : watchedFds)
        juce::LinuxEventLoop::unregisterFdCallback(fd);
#endif
    host->owner = nullptr;
    plugin->destroy(plugin);
}

void ClapPluginInstance::readExtensions()
{
    const auto ext = [this](const char* id) { return plugin->get_extension(plugin, id); };
    params = static_cast<const clap_plugin_params_t*>(ext(CLAP_EXT_PARAMS));
    state = static_cast<const clap_plugin_state_t*>(ext(CLAP_EXT_STATE));
    latency = static_cast<const clap_plugin_latency_t*>(ext(CLAP_EXT_LATENCY));
    tail = static_cast<const clap_plugin_tail_t*>(ext(CLAP_EXT_TAIL));
    gui = static_cast<const clap_plugin_gui_t*>(ext(CLAP_EXT_GUI));
    timers = static_cast<const clap_plugin_timer_support_t*>(ext(CLAP_EXT_TIMER_SUPPORT));
    fds = static_cast<const clap_plugin_posix_fd_support_t*>(ext(CLAP_EXT_POSIX_FD_SUPPORT));
    audioPorts = static_cast<const clap_plugin_audio_ports_t*>(ext(CLAP_EXT_AUDIO_PORTS));
    notePorts = static_cast<const clap_plugin_note_ports_t*>(ext(CLAP_EXT_NOTE_PORTS));

    if (notePorts != nullptr)
    {
        noteInputs = static_cast<int>(notePorts->count(plugin, true));
        noteOutputs = static_cast<int>(notePorts->count(plugin, false));
        clap_note_port_info_t port{};
        if (noteInputs > 0 && notePorts->get(plugin, 0, true, &port))
        {
            clapNotes = (port.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0;
            midiNotes = (port.supported_dialects & (CLAP_NOTE_DIALECT_MIDI | CLAP_NOTE_DIALECT_MIDI_MPE)) != 0;
            mpeMidi = (port.supported_dialects & CLAP_NOTE_DIALECT_MIDI_MPE) != 0;
        }
    }

    // Port channel counts, and where each port's channels are in JUCE's buffer.
    for (const bool isInput : {true, false})
    {
        auto& counts = isInput ? inputChannels : outputChannels;
        auto& pointers = isInput ? inputPointers : outputPointers;
        auto& buffers = isInput ? inputBuffers : outputBuffers;
        counts.clear();
        for (int b = 0; b < getBusCount(isInput); ++b)
            counts.push_back(getBus(isInput, b)->getNumberOfChannels());
        pointers.assign(counts.size(), {});
        buffers.assign(counts.size(), clap_audio_buffer_t{});
        for (std::size_t b = 0; b < counts.size(); ++b)
        {
            pointers[b].assign(static_cast<std::size_t>(counts[b]), nullptr);
            buffers[b].data32 = pointers[b].data();
            buffers[b].channel_count = static_cast<std::uint32_t>(counts[b]);
        }
    }
}

void ClapPluginInstance::readParameters()
{
    if (params == nullptr)
        return;

    // The module paths ("Osc/Osc 1") become groups, in the order the parameters come in.
    struct Node
    {
        juce::String name, path;
        std::vector<std::variant<std::unique_ptr<Node>, std::unique_ptr<ClapParameter>>> children;
        Node* child(const juce::String& childName)
        {
            for (auto& c : children)
                if (auto* n = std::get_if<std::unique_ptr<Node>>(&c); n != nullptr && (*n)->name == childName)
                    return n->get();
            auto made = std::make_unique<Node>();
            made->name = childName;
            made->path = path.isEmpty() ? childName : path + "/" + childName;
            auto* raw = made.get();
            children.emplace_back(std::move(made));
            return raw;
        }
    };
    Node root;
    const auto count = params->count(plugin);
    for (std::uint32_t i = 0; i < count; ++i)
    {
        clap_param_info_t paramInfo{};
        if (!params->get_info(plugin, i, &paramInfo))
            continue;
        double value = paramInfo.default_value;
        params->get_value(plugin, paramInfo.id, &value);
        auto parameter = std::make_unique<ClapParameter>(*this, paramInfo, value);
        Node* at = &root;
        for (const auto& part : juce::StringArray::fromTokens(parameter->module, "/", {}))
            if (part.trim().isNotEmpty())
                at = at->child(part.trim());
        at->children.emplace_back(std::move(parameter));
    }

    std::function<void(Node&, juce::AudioProcessorParameterGroup&)> build = [&](Node& node, juce::AudioProcessorParameterGroup& group) {
        for (auto& c : node.children)
        {
            if (auto* n = std::get_if<std::unique_ptr<Node>>(&c))
            {
                auto sub = std::make_unique<juce::AudioProcessorParameterGroup>((*n)->path, (*n)->name, "/");
                build(**n, *sub);
                group.addChild(std::move(sub));
            }
            else
                group.addChild(std::move(std::get<std::unique_ptr<ClapParameter>>(c)));
        }
    };
    juce::AudioProcessorParameterGroup tree;
    build(root, tree);
    setHostedParameterTree(std::move(tree));

    for (auto* p : getParameters())
        if (auto* cp = dynamic_cast<ClapParameter*>(p))
        {
            cp->index = static_cast<int>(parameters.size());
            indexOfId[cp->id] = cp->index;
            parameters.push_back(cp);
        }
    const auto n = parameters.size();
    pendingValue = std::make_unique<std::atomic<double>[]>(n);
    pendingFlag = std::make_unique<std::atomic<bool>[]>(n);
    reportedValue = std::make_unique<std::atomic<bool>[]>(n);
    reportedGestures = std::make_unique<std::atomic<int>[]>(n);
    gestureOpen.assign(n, false);
}

void ClapPluginInstance::refreshParameterInfo()
{
    if (params == nullptr)
        return;
    for (std::uint32_t i = 0; i < params->count(plugin); ++i)
    {
        clap_param_info_t paramInfo{};
        if (params->get_info(plugin, i, &paramInfo))
            if (const auto it = indexOfId.find(paramInfo.id); it != indexOfId.end())
                parameters[static_cast<std::size_t>(it->second)]->setInfo(paramInfo);
    }
}

void ClapPluginInstance::fillInPluginDescription(juce::PluginDescription& description) const
{
    description = info;
}

// --- Activation ----------------------------------------------------------------------------------------------

void ClapPluginInstance::prepareToPlay(double sampleRate, int maximumExpectedSamplesPerBlock)
{
    JUCE_ASSERT_MESSAGE_THREAD
    deactivate();
    const int maxBlock = juce::jmax(1, maximumExpectedSamplesPerBlock);
    setRateAndBufferSizeDetails(sampleRate, maxBlock);

    int inputTotal = 0;
    for (const int c : inputChannels)
        inputTotal += c;
    inputScratch.setSize(juce::jmax(1, inputTotal), maxBlock, false, true, false);
    inputScratch.clear();
    inEvents->prepare(256 * 1024, 8192);
    outEvents->prepare(256 * 1024, 8192);
    mainIn->prepare(64 * 1024, 4096);
    mainOut->prepare(64 * 1024, 4096);
    for (auto& channel : noteIds)
        channel.fill(-1);
    channelBend.fill(0.0f);
    channelPressure.fill(0.0f);
    channelTimbre.fill(0.5f);
    steadyTime = 0;

    // Where each port's channels are in the buffer JUCE gives processBlock().
    for (const bool isInput : {true, false})
    {
        auto& index = isInput ? inputIndex : outputIndex;
        const auto& counts = isInput ? inputChannels : outputChannels;
        index.assign(counts.size(), {});
        for (std::size_t b = 0; b < counts.size(); ++b)
            for (int c = 0; c < counts[b]; ++c)
                index[b].push_back(getChannelIndexInProcessBlockBuffer(isInput, static_cast<int>(b), c));
    }
    outputSpare.setSize(juce::jmax(1, std::accumulate(outputChannels.begin(), outputChannels.end(), 0)), maxBlock, false, true, false);

    host->latencyChanged = false;
    bool ok = false;
    withoutErrorBoxes([&] { ok = plugin->activate(plugin, sampleRate, 1, static_cast<std::uint32_t>(maxBlock)); });
    active.store(ok);
    // Latency may be asked for once activated, and may change during activate().
    setLatencySamples(ok && latency != nullptr ? static_cast<int>(latency->get(plugin)) : 0);
    host->latencyChanged = false;
}

void ClapPluginInstance::releaseResources()
{
    deactivate();
}

void ClapPluginInstance::deactivate()
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (processing.exchange(false))
    {
        const ActingAsAudioThread acting;
        plugin->stop_processing(plugin);
    }
    if (active.exchange(false))
        plugin->deactivate(plugin);
}

bool ClapPluginInstance::isBusesLayoutSupported(const BusesLayout& layout) const
{
    // The ports' own layout only: CLAP has no way to turn a port's channels off.
    if (layout.inputBuses.size() != static_cast<int>(inputChannels.size()) ||
        layout.outputBuses.size() != static_cast<int>(outputChannels.size()))
        return false;
    for (int b = 0; b < layout.inputBuses.size(); ++b)
        if (layout.inputBuses[b].size() != inputChannels[static_cast<std::size_t>(b)])
            return false;
    for (int b = 0; b < layout.outputBuses.size(); ++b)
        if (layout.outputBuses[b].size() != outputChannels[static_cast<std::size_t>(b)])
            return false;
    return true;
}

double ClapPluginInstance::getTailLengthSeconds() const
{
    if (tail == nullptr || getSampleRate() <= 0.0)
        return 0.0;
    const auto samples = tail->get(plugin);
    return samples == std::numeric_limits<std::uint32_t>::max() ? std::numeric_limits<double>::infinity()
                                                                : static_cast<double>(samples) / getSampleRate();
}

// --- Processing ----------------------------------------------------------------------------------------------

void ClapPluginInstance::queueValue(int index, double plainValue) noexcept
{
    if (index < 0 || index >= static_cast<int>(parameters.size()))
        return;
    pendingValue[static_cast<std::size_t>(index)].store(plainValue, std::memory_order_relaxed);
    pendingFlag[static_cast<std::size_t>(index)].store(true, std::memory_order_release);
    anyPending.store(true, std::memory_order_release);
}

void ClapPluginInstance::addParameterChanges(Events& in) noexcept
{
    if (!anyPending.exchange(false, std::memory_order_acq_rel))
        return;
    for (std::size_t i = 0; i < parameters.size(); ++i)
    {
        if (!pendingFlag[i].exchange(false, std::memory_order_acq_rel))
            continue;
        const auto* p = parameters[i];
        clap_event_param_value_t e{};
        e.header = {sizeof(e), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        e.param_id = p->id;
        e.cookie = p->cookie;
        e.note_id = -1;
        e.port_index = -1;
        e.channel = -1;
        e.key = -1;
        e.value = pendingValue[i].load(std::memory_order_relaxed);
        if (!in.add(e))
        {
            // Full: the rest go next time.
            pendingFlag[i].store(true, std::memory_order_relaxed);
            anyPending.store(true, std::memory_order_relaxed);
        }
    }
}

void ClapPluginInstance::addTransport(Events&) noexcept
{
    transport = {};
    transport.header = {sizeof(transport), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_TRANSPORT, 0};
    auto* head = getPlayHead();
    if (head == nullptr)
        return;
    const auto position = head->getPosition();
    if (!position.hasValue())
        return;
    if (const auto bpm = position->getBpm())
    {
        transport.flags |= CLAP_TRANSPORT_HAS_TEMPO;
        transport.tempo = *bpm;
    }
    if (const auto ppq = position->getPpqPosition())
    {
        transport.flags |= CLAP_TRANSPORT_HAS_BEATS_TIMELINE;
        transport.song_pos_beats = static_cast<clap_beattime>(std::llround(*ppq * static_cast<double>(CLAP_BEATTIME_FACTOR)));
        if (const auto bar = position->getPpqPositionOfLastBarStart())
            transport.bar_start = static_cast<clap_beattime>(std::llround(*bar * static_cast<double>(CLAP_BEATTIME_FACTOR)));
        if (const auto bars = position->getBarCount())
            transport.bar_number = static_cast<std::int32_t>(*bars);
    }
    if (const auto seconds = position->getTimeInSeconds())
    {
        transport.flags |= CLAP_TRANSPORT_HAS_SECONDS_TIMELINE;
        transport.song_pos_seconds = static_cast<clap_sectime>(std::llround(*seconds * static_cast<double>(CLAP_SECTIME_FACTOR)));
    }
    if (const auto sig = position->getTimeSignature())
    {
        transport.flags |= CLAP_TRANSPORT_HAS_TIME_SIGNATURE;
        transport.tsig_num = static_cast<std::uint16_t>(sig->numerator);
        transport.tsig_denom = static_cast<std::uint16_t>(sig->denominator);
    }
    if (position->getIsPlaying())
        transport.flags |= CLAP_TRANSPORT_IS_PLAYING;
    if (position->getIsRecording())
        transport.flags |= CLAP_TRANSPORT_IS_RECORDING;
}

void ClapPluginInstance::addMidi(Events& in, const juce::MidiBuffer& midi, int numSamples) noexcept
{
    const bool mpe = mpeInput.load(std::memory_order_relaxed);
    const auto expression = [&](std::uint32_t time, clap_note_expression what, int channel, int key, double value) {
        clap_event_note_expression_t e{};
        e.header = {sizeof(e), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_EXPRESSION, 0};
        e.expression_id = what;
        e.note_id = noteIds[static_cast<std::size_t>(channel)][static_cast<std::size_t>(key)];
        e.port_index = 0;
        e.channel = static_cast<std::int16_t>(channel);
        e.key = static_cast<std::int16_t>(key);
        e.value = value;
        in.add(e);
    };

    for (const auto meta : midi)
    {
        if (meta.numBytes < 1 || meta.numBytes > 3)
            continue; // sysex isn't passed on
        const auto* d = meta.data;
        const auto time = static_cast<std::uint32_t>(juce::jlimit(0, juce::jmax(0, numSamples - 1), meta.samplePosition));
        const int type = d[0] & 0xf0, channel = d[0] & 0x0f;
        const int key = meta.numBytes > 1 ? d[1] & 0x7f : 0;
        const int value = meta.numBytes > 2 ? d[2] & 0x7f : 0;

        if (clapNotes && (type == 0x90 || type == 0x80))
        {
            auto& id = noteIds[static_cast<std::size_t>(channel)][static_cast<std::size_t>(key)];
            clap_event_note_t e{};
            e.header = {sizeof(e), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_OFF, 0};
            e.port_index = 0;
            e.channel = static_cast<std::int16_t>(channel);
            e.key = static_cast<std::int16_t>(key);
            e.velocity = value / 127.0;
            if (type == 0x90 && value > 0)
            {
                id = nextNoteId;
                nextNoteId = nextNoteId == std::numeric_limits<std::int32_t>::max() ? 0 : nextNoteId + 1;
                e.header.type = CLAP_EVENT_NOTE_ON;
                e.note_id = id;
                in.add(e);
                // A member channel's expression, sent before its note-on, applies from the start.
                if (mpe && channel != 0)
                {
                    const auto c = static_cast<std::size_t>(channel);
                    if (channelBend[c] != 0.0f)
                        expression(time, CLAP_NOTE_EXPRESSION_TUNING, channel, key, channelBend[c] * perNoteSemitones);
                    if (channelPressure[c] != 0.0f)
                        expression(time, CLAP_NOTE_EXPRESSION_PRESSURE, channel, key, channelPressure[c]);
                    if (channelTimbre[c] != 0.5f)
                        expression(time, CLAP_NOTE_EXPRESSION_BRIGHTNESS, channel, key, channelTimbre[c]);
                }
            }
            else
            {
                e.note_id = id;
                id = -1;
                in.add(e);
            }
            continue;
        }
        if (clapNotes && type == 0xa0)
        {
            expression(time, CLAP_NOTE_EXPRESSION_PRESSURE, channel, key, value / 127.0);
            continue;
        }
        if (mpe && clapNotes && channel != 0 && (type == 0xe0 || type == 0xd0 || (type == 0xb0 && key == 74)))
        {
            const auto c = static_cast<std::size_t>(channel);
            clap_note_expression what = CLAP_NOTE_EXPRESSION_TUNING;
            double v = 0.0;
            if (type == 0xe0)
            {
                channelBend[c] = juce::jlimit(-1.0f, 1.0f, static_cast<float>(((value << 7) | key) - 8192) / 8191.0f);
                v = channelBend[c] * perNoteSemitones;
            }
            else if (type == 0xd0)
            {
                what = CLAP_NOTE_EXPRESSION_PRESSURE;
                channelPressure[c] = static_cast<float>(key) / 127.0f; // a channel pressure's value is its first data byte
                v = channelPressure[c];
            }
            else
            {
                what = CLAP_NOTE_EXPRESSION_BRIGHTNESS;
                channelTimbre[c] = static_cast<float>(value) / 127.0f;
                v = channelTimbre[c];
            }
            for (int k = 0; k < 128; ++k)
                if (noteIds[c][static_cast<std::size_t>(k)] >= 0)
                    expression(time, what, channel, k, v);
            continue;
        }
        if (midiNotes || !clapNotes)
        {
            clap_event_midi_t e{};
            e.header = {sizeof(e), time, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_MIDI, 0};
            e.port_index = 0;
            std::copy_n(d, meta.numBytes, e.data);
            in.add(e);
        }
    }
}

void ClapPluginInstance::readOutput(const Events& out, juce::MidiBuffer& midi) noexcept
{
    for (std::uint32_t i = 0; i < out.size(); ++i)
    {
        const auto* h = out.get(i);
        if (h == nullptr || h->space_id != CLAP_CORE_EVENT_SPACE_ID)
            continue;
        switch (h->type)
        {
        case CLAP_EVENT_PARAM_VALUE:
        {
            const auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
            if (const auto it = indexOfId.find(e->param_id); it != indexOfId.end())
            {
                const auto index = static_cast<std::size_t>(it->second);
                parameters[index]->setFromPlugin(e->value);
                reportedValue[index].store(true, std::memory_order_release);
                anyReport.store(true, std::memory_order_release);
            }
            break;
        }
        case CLAP_EVENT_PARAM_GESTURE_BEGIN:
        case CLAP_EVENT_PARAM_GESTURE_END:
        {
            const auto* e = reinterpret_cast<const clap_event_param_gesture_t*>(h);
            if (const auto it = indexOfId.find(e->param_id); it != indexOfId.end())
            {
                reportedGestures[static_cast<std::size_t>(it->second)].fetch_or(h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN ? 1 : 2);
                anyReport.store(true, std::memory_order_release);
            }
            break;
        }
        case CLAP_EVENT_NOTE_ON:
        case CLAP_EVENT_NOTE_OFF:
        {
            const auto* e = reinterpret_cast<const clap_event_note_t*>(h);
            if (e->key < 0 || e->key > 127)
                break;
            const auto channel = static_cast<std::uint8_t>(juce::jlimit(0, 15, static_cast<int>(e->channel)));
            const auto velocity = static_cast<std::uint8_t>(juce::jlimit(h->type == CLAP_EVENT_NOTE_ON ? 1 : 0, 127,
                                                                         static_cast<int>(std::lround(e->velocity * 127.0))));
            const std::uint8_t bytes[3]{static_cast<std::uint8_t>((h->type == CLAP_EVENT_NOTE_ON ? 0x90 : 0x80) | channel),
                                        static_cast<std::uint8_t>(e->key), velocity};
            midi.addEvent(bytes, 3, static_cast<int>(h->time));
            break;
        }
        case CLAP_EVENT_MIDI:
        {
            const auto* e = reinterpret_cast<const clap_event_midi_t*>(h);
            const int length = juce::MidiMessage::getMessageLengthFromFirstByte(e->data[0]);
            if (length >= 1 && length <= 3)
                midi.addEvent(e->data, length, static_cast<int>(h->time));
            break;
        }
        default:
            break;
        }
    }
}

void ClapPluginInstance::processBlock(juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midi) noexcept
{
    const int n = buffer.getNumSamples();
    if (!active.load(std::memory_order_acquire) || n <= 0 || n > inputScratch.getNumSamples())
    {
        buffer.clear();
        midi.clear();
        return;
    }
    if (!processing.load(std::memory_order_relaxed))
    {
        if (!plugin->start_processing(plugin))
        {
            buffer.clear();
            midi.clear();
            return;
        }
        processing.store(true, std::memory_order_relaxed);
    }

    // Inputs into their own buffers (CLAP's inputs and outputs are separate unless paired).
    int scratch = 0;
    for (std::size_t b = 0; b < inputIndex.size(); ++b)
        for (std::size_t c = 0; c < inputIndex[b].size(); ++c)
        {
            float* to = inputScratch.getWritePointer(scratch++);
            const int at = inputIndex[b][c];
            if (at >= 0 && at < buffer.getNumChannels())
                std::copy_n(buffer.getReadPointer(at), n, to);
            else
                std::fill_n(to, n, 0.0f);
            inputPointers[b][c] = to;
        }
    int spare = 0;
    for (std::size_t b = 0; b < outputIndex.size(); ++b)
        for (std::size_t c = 0; c < outputIndex[b].size(); ++c)
        {
            const int at = outputIndex[b][c];
            outputPointers[b][c] = at >= 0 && at < buffer.getNumChannels() ? buffer.getWritePointer(at) : outputSpare.getWritePointer(spare);
            ++spare;
        }

    inEvents->clear();
    outEvents->clear();
    addTransport(*inEvents);
    addParameterChanges(*inEvents);
    addMidi(*inEvents, midi, n);

    clap_process_t process{};
    process.steady_time = steadyTime;
    process.frames_count = static_cast<std::uint32_t>(n);
    process.transport = (transport.flags != 0) ? &transport : nullptr;
    process.audio_inputs = inputBuffers.data();
    process.audio_inputs_count = static_cast<std::uint32_t>(inputBuffers.size());
    process.audio_outputs = outputBuffers.data();
    process.audio_outputs_count = static_cast<std::uint32_t>(outputBuffers.size());
    process.in_events = &inEvents->input;
    process.out_events = &outEvents->output;
    const auto status = plugin->process(plugin, &process);
    steadyTime += n;
    if (status == CLAP_PROCESS_ERROR)
        buffer.clear();

    midi.clear();
    readOutput(*outEvents, midi);
}

void ClapPluginInstance::reset() noexcept
{
    if (active.load(std::memory_order_acquire))
        plugin->reset(plugin);
}

// --- State -------------------------------------------------------------------------------------------------

void ClapPluginInstance::getStateInformation(juce::MemoryBlock& destination)
{
    JUCE_ASSERT_MESSAGE_THREAD
    destination.reset();
    if (state == nullptr)
        return;
    juce::MemoryOutputStream out(destination, false);
    clap_ostream_t stream{&out, [](const clap_ostream_t* s, const void* data, std::uint64_t size) -> std::int64_t {
                              return static_cast<juce::MemoryOutputStream*>(s->ctx)->write(data, static_cast<std::size_t>(size))
                                         ? static_cast<std::int64_t>(size)
                                         : -1;
                          }};
    if (!state->save(plugin, &stream))
        destination.reset();
}

void ClapPluginInstance::setStateInformation(const void* data, int sizeInBytes)
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (state == nullptr || data == nullptr || sizeInBytes <= 0)
        return;
    juce::MemoryInputStream in(data, static_cast<std::size_t>(sizeInBytes), false);
    clap_istream_t stream{&in, [](const clap_istream_t* s, void* buffer, std::uint64_t size) -> std::int64_t {
                              return static_cast<juce::MemoryInputStream*>(s->ctx)->read(buffer, static_cast<int>(juce::jmin<std::uint64_t>(size, 1u << 30)));
                          }};
    state->load(plugin, &stream);
    // The values it loaded (a plugin doesn't announce them).
    if (params != nullptr)
        for (auto* p : parameters)
        {
            double v = 0.0;
            if (params->get_value(plugin, p->id, &v))
                p->setFromPlugin(v);
        }
}

// --- Text ----------------------------------------------------------------------------------------------------

juce::String ClapPluginInstance::valueToText(int index, double plainValue) const
{
    if (index >= 0 && index < static_cast<int>(parameters.size()) && params != nullptr && params->value_to_text != nullptr &&
        juce::MessageManager::existsAndIsCurrentThread())
    {
        char text[256]{};
        if (params->value_to_text(plugin, parameters[static_cast<std::size_t>(index)]->id, plainValue, text, sizeof(text)))
            return textOf(text);
    }
    return juce::String(plainValue, 2);
}

std::optional<double> ClapPluginInstance::textToValue(int index, const juce::String& text) const
{
    if (index < 0 || index >= static_cast<int>(parameters.size()) || params == nullptr || params->text_to_value == nullptr ||
        !juce::MessageManager::existsAndIsCurrentThread())
        return std::nullopt;
    double v = 0.0;
    if (params->text_to_value(plugin, parameters[static_cast<std::size_t>(index)]->id, text.toRawUTF8(), &v))
        return v;
    return std::nullopt;
}

// --- The message thread's work -------------------------------------------------------------------------------

void ClapPluginInstance::timerCallback()
{
    idle();
}

void ClapPluginInstance::idle()
{
    JUCE_ASSERT_MESSAGE_THREAD
    if (host->callback.exchange(false))
        plugin->on_main_thread(plugin);

    if (const auto flags = host->rescanFlags.exchange(0); flags != 0 && params != nullptr)
    {
        if ((flags & (CLAP_PARAM_RESCAN_INFO | CLAP_PARAM_RESCAN_ALL)) != 0)
            refreshParameterInfo();
        if ((flags & (CLAP_PARAM_RESCAN_VALUES | CLAP_PARAM_RESCAN_ALL)) != 0)
            for (auto* p : parameters)
            {
                double v = 0.0;
                if (params->get_value(plugin, p->id, &v) && p->toNormalised(v) != p->getValue())
                {
                    p->setFromPlugin(v);
                    reportedValue[static_cast<std::size_t>(p->index)].store(true);
                    anyReport.store(true);
                }
            }
        updateHostDisplay(ChangeDetails().withParameterInfoChanged(true));
    }

    // Not active: its parameter changes go through flush() here (while active, only process() may).
    if (!active.load() && (anyPending.load() || host->flush.load()) && params != nullptr)
    {
        host->flush = false;
        mainIn->clear();
        mainOut->clear();
        addParameterChanges(*mainIn);
        params->flush(plugin, &mainIn->input, &mainOut->output);
        juce::MidiBuffer ignored;
        readOutput(*mainOut, ignored);
    }

    deliverReports();

    if (host->stateDirty.exchange(false))
        updateHostDisplay(ChangeDetails().withNonParameterStateChanged(true));

    // A latency change while active needs a restart (CLAP allows it only while deactivated).
    if (host->latencyChanged.exchange(false) && active.load())
        host->restart = true;
    if (host->restart.exchange(false) && onRestartRequested)
        onRestartRequested();

    if (host->resizeRequested.exchange(false) && editor != nullptr)
        editor->pluginAskedForSize(host->resizeWidth.load(), host->resizeHeight.load());
    if (host->closed.exchange(false) && onEditorClosed)
        onEditorClosed();
}

void ClapPluginInstance::deliverReports()
{
    if (!anyReport.exchange(false, std::memory_order_acq_rel))
        return;
    for (std::size_t i = 0; i < parameters.size(); ++i)
    {
        const int gestures = reportedGestures[i].exchange(0, std::memory_order_acq_rel);
        const bool changed = reportedValue[i].exchange(false, std::memory_order_acq_rel);
        auto* p = parameters[i];
        if ((gestures & 1) != 0 && !gestureOpen[i])
        {
            gestureOpen[i] = true;
            p->beginChangeGesture();
        }
        if (changed)
            p->sendValueChangedMessageToListeners(p->getValue());
        if ((gestures & 2) != 0 && gestureOpen[i])
        {
            gestureOpen[i] = false;
            p->endChangeGesture();
        }
    }
}

bool ClapPluginInstance::registerTimer(std::uint32_t periodMs, clap_id& id)
{
    id = nextTimerId++;
    auto timer = std::make_unique<PluginTimer>(*this, id);
    timer->startTimer(static_cast<int>(juce::jmax<std::uint32_t>(1, periodMs)));
    pluginTimers[id] = std::move(timer);
    return true;
}

bool ClapPluginInstance::unregisterTimer(clap_id id)
{
    return pluginTimers.erase(id) > 0;
}

#if JUCE_LINUX || JUCE_BSD
bool ClapPluginInstance::watchFd(int fd, std::uint32_t flags)
{
    if (fds == nullptr)
        return false;
    short events = 0;
    if ((flags & CLAP_POSIX_FD_READ) != 0)
        events |= POLLIN;
    if ((flags & CLAP_POSIX_FD_WRITE) != 0)
        events |= POLLOUT;
    if ((flags & CLAP_POSIX_FD_ERROR) != 0)
        events |= POLLERR;
    juce::LinuxEventLoop::registerFdCallback(
        fd, [this, flags](int f) { fds->on_fd(plugin, f, flags); }, events);
    watchedFds.push_back(fd);
    return true;
}

bool ClapPluginInstance::unwatchFd(int fd)
{
    const auto it = std::find(watchedFds.begin(), watchedFds.end(), fd);
    if (it == watchedFds.end())
        return false;
    juce::LinuxEventLoop::unregisterFdCallback(fd);
    watchedFds.erase(it);
    return true;
}
#endif

// --- The window ---------------------------------------------------------------------------------------------

bool ClapPluginInstance::hasEditor() const
{
    return gui != nullptr && windowApi != nullptr && gui->is_api_supported(plugin, windowApi, false);
}

juce::AudioProcessorEditor* ClapPluginInstance::createEditor()
{
    if (!hasEditor())
        return nullptr;
    auto* made = new ClapEditor(*this, windowApi);
    editor = made;
    return made;
}

} // namespace lattice::hosting
