// CLAP test plugins (docs/07-m6-plugins-external.md, "Tests" 1, 2): several plugins in one file, as
// CLAP allows, for the scan and for hosting. Built only for the tests; never copied into a system
// plugin folder.
//
// - "Lattice Test Plugin": a sine synth (stereo out; notes in CLAP's dialect, MIDI or MPE). Its
//   parameters: Level, Wave (stepped) and a hidden one. Note expressions tune each note (in
//   semitones), so a host's per-note pitch can be measured in its output. CC 20 sets Level as
//   if from its own window (with a gesture, reported to the host); a program change sets its
//   patch number, state that isn't a parameter (reported with mark_dirty). It has the name and
//   vendor of the JUCE-built VST3, so the two are one plugin in two formats. It has a window that
//   draws nothing, and refuses a second one while the first isn't destroyed, as real plugins do.
// - "Lattice Test Clap Arp": a note effect (no audio): every note comes out, and the same note an
//   octave up.
// - "Lattice Test Latency": an effect that delays its input by its Latency parameter, and reports
//   it. Changed while active, it asks for a restart, as CLAP says it must.
// - "Lattice Test Sidechain": an effect with a sidechain port: out = main + sidechain.
// - "Lattice Test Allocator": passes audio through, and allocates on the audio thread every block.

#include <clap/clap.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

#if defined(_WIN32)
#define LATTICE_EXPORT extern "C" __declspec(dllexport)
#else
#define LATTICE_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace
{
const char* const synthFeatures[] = {CLAP_PLUGIN_FEATURE_INSTRUMENT, CLAP_PLUGIN_FEATURE_SYNTHESIZER, CLAP_PLUGIN_FEATURE_STEREO,
                                     nullptr};
const char* const arpFeatures[] = {CLAP_PLUGIN_FEATURE_NOTE_EFFECT, "arpeggiator", nullptr};
const char* const effectFeatures[] = {CLAP_PLUGIN_FEATURE_AUDIO_EFFECT, CLAP_PLUGIN_FEATURE_UTILITY, nullptr};

enum class Kind
{
    synth,
    arp,
    latency,
    sidechain,
    allocator
};

const clap_plugin_descriptor_t descriptors[] = {
    {CLAP_VERSION_INIT, "org.lattice.tests.clap-synth", "Lattice Test Plugin", "Lattice Tests", "", "", "", "1.2.3",
     "A CLAP sine synth", synthFeatures},
    {CLAP_VERSION_INIT, "org.lattice.tests.clap-arp", "Lattice Test Clap Arp", "Lattice Tests", "", "", "", "0.9", "", arpFeatures},
    {CLAP_VERSION_INIT, "org.lattice.tests.clap-latency", "Lattice Test Latency", "Lattice Tests", "", "", "", "1.0", "",
     effectFeatures},
    {CLAP_VERSION_INIT, "org.lattice.tests.clap-sidechain", "Lattice Test Sidechain", "Lattice Tests", "", "", "", "1.0", "",
     effectFeatures},
    {CLAP_VERSION_INIT, "org.lattice.tests.clap-allocator", "Lattice Test Allocator", "Lattice Tests", "", "", "", "1.0", "",
     effectFeatures},
};
constexpr std::uint32_t descriptorCount = sizeof(descriptors) / sizeof(descriptors[0]);

struct Param
{
    clap_id id;
    const char* name;
    const char* module;
    double min, max, def;
    std::uint32_t flags;
};

const Param synthParams[] = {
    {10, "Level", "Output", 0.0, 1.0, 0.25, CLAP_PARAM_IS_AUTOMATABLE},
    {20, "Wave", "Oscillator", 0.0, 2.0, 0.0, CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED | CLAP_PARAM_IS_ENUM},
    {30, "Hidden", "", 0.0, 1.0, 0.0, CLAP_PARAM_IS_HIDDEN},
};
const Param latencyParams[] = {
    {1, "Latency", "", 0.0, 4000.0, 64.0, CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED},
};

struct Plugin
{
    clap_plugin_t clap{};
    const clap_host_t* host = nullptr;
    const clap_host_params_t* hostParams = nullptr;
    const clap_host_state_t* hostState = nullptr;
    const clap_host_latency_t* hostLatency = nullptr;
    Kind kind = Kind::synth;

    std::vector<Param> params;
    std::vector<double> values;

    double rate = 48000.0;
    bool active = false;
    int patch = 0; // state that isn't a parameter (synth)

    // Synth voices: one per channel and key
    struct Voice
    {
        bool on = false;
        double phase = 0.0;
        double tuning = 0.0; // semitones, from note expressions
        int key = 0;
        int channel = 0;
        std::int32_t id = -1;
    };
    Voice voices[16][128]{};

    bool guiCreated = false;

    // Latency effect
    int activeLatency = 0;
    std::vector<float> ring[2];
    std::size_t ringPos = 0;
    std::atomic<bool> restartAsked{false};

    // Pending output (synth's CC 20)
    bool sendLevel = false;
    double levelToSend = 0.0;

    int indexOf(clap_id id) const
    {
        for (std::size_t i = 0; i < params.size(); ++i)
            if (params[i].id == id)
                return static_cast<int>(i);
        return -1;
    }
    double value(clap_id id) const
    {
        const int i = indexOf(id);
        return i >= 0 ? values[static_cast<std::size_t>(i)] : 0.0;
    }

    void setParam(clap_id id, double v)
    {
        const int i = indexOf(id);
        if (i < 0)
            return;
        values[static_cast<std::size_t>(i)] = v;
        if (kind == Kind::latency && id == 1 && active && static_cast<int>(v) != activeLatency && !restartAsked.exchange(true))
            host->request_restart(host);
    }
};

Plugin& self(const clap_plugin_t* p) { return *static_cast<Plugin*>(p->plugin_data); }

// --- Ports ---------------------------------------------------------------------------------------
std::uint32_t audioCount(const clap_plugin_t* p, bool isInput)
{
    switch (self(p).kind)
    {
    case Kind::synth: return isInput ? 0u : 1u;
    case Kind::arp: return 0u;
    case Kind::sidechain: return isInput ? 2u : 1u;
    case Kind::latency:
    case Kind::allocator: return 1u;
    }
    return 0u;
}
bool audioGet(const clap_plugin_t* p, std::uint32_t index, bool isInput, clap_audio_port_info_t* info)
{
    if (index >= audioCount(p, isInput))
        return false;
    info->id = index;
    std::strcpy(info->name, isInput ? (index == 0 ? "In" : "Sidechain") : "Out");
    info->flags = index == 0 ? CLAP_AUDIO_PORT_IS_MAIN : 0u;
    info->channel_count = 2;
    info->port_type = CLAP_PORT_STEREO;
    info->in_place_pair = CLAP_INVALID_ID;
    return true;
}
const clap_plugin_audio_ports_t audioPorts{audioCount, audioGet};

std::uint32_t noteCount(const clap_plugin_t* p, bool isInput)
{
    switch (self(p).kind)
    {
    case Kind::synth: return isInput ? 1u : 0u;
    case Kind::arp: return 1u;
    default: return 0u;
    }
}
bool noteGet(const clap_plugin_t* p, std::uint32_t index, bool isInput, clap_note_port_info_t* info)
{
    if (index >= noteCount(p, isInput))
        return false;
    const bool synth = self(p).kind == Kind::synth;
    info->id = 0;
    info->supported_dialects = CLAP_NOTE_DIALECT_CLAP | CLAP_NOTE_DIALECT_MIDI | (synth && isInput ? CLAP_NOTE_DIALECT_MIDI_MPE : 0u);
    info->preferred_dialect = CLAP_NOTE_DIALECT_CLAP;
    std::strcpy(info->name, isInput ? "Notes In" : "Notes Out");
    return true;
}
const clap_plugin_note_ports_t notePorts{noteCount, noteGet};

// --- Parameters -----------------------------------------------------------------------------------
std::uint32_t paramCount(const clap_plugin_t* p) { return static_cast<std::uint32_t>(self(p).params.size()); }
bool paramInfo(const clap_plugin_t* p, std::uint32_t index, clap_param_info_t* info)
{
    const auto& params = self(p).params;
    if (index >= params.size())
        return false;
    const auto& d = params[index];
    *info = {};
    info->id = d.id;
    info->flags = d.flags;
    std::strncpy(info->name, d.name, sizeof(info->name) - 1);
    std::strncpy(info->module, d.module, sizeof(info->module) - 1);
    info->min_value = d.min;
    info->max_value = d.max;
    info->default_value = d.def;
    return true;
}
bool paramValue(const clap_plugin_t* p, clap_id id, double* out)
{
    if (self(p).indexOf(id) < 0)
        return false;
    *out = self(p).value(id);
    return true;
}
bool paramToText(const clap_plugin_t* p, clap_id id, double value, char* out, std::uint32_t size)
{
    std::string text;
    if (self(p).kind == Kind::synth && id == 20)
    {
        static const char* const waves[] = {"Sine", "Square", "Saw"};
        text = waves[static_cast<int>(std::lround(value)) % 3];
    }
    else if (self(p).kind == Kind::synth && id == 10)
        text = std::to_string(static_cast<int>(std::lround(value * 100.0))) + " %";
    else
        text = std::to_string(static_cast<int>(std::lround(value))) + " smp";
    std::strncpy(out, text.c_str(), size - 1);
    out[size - 1] = 0;
    return true;
}
bool paramFromText(const clap_plugin_t*, clap_id, const char* text, double* out)
{
    *out = std::atof(text);
    return true;
}
void applyParamEvents(Plugin& s, const clap_input_events_t* in)
{
    for (std::uint32_t i = 0; in != nullptr && i < in->size(in); ++i)
    {
        const auto* h = in->get(in, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
        {
            const auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
            s.setParam(e->param_id, e->value);
        }
    }
}
void paramFlush(const clap_plugin_t* p, const clap_input_events_t* in, const clap_output_events_t*)
{
    applyParamEvents(self(p), in);
}
const clap_plugin_params_t paramsExt{paramCount, paramInfo, paramValue, paramToText, paramFromText, paramFlush};

// --- State ----------------------------------------------------------------------------------------
bool stateSave(const clap_plugin_t* p, const clap_ostream_t* stream)
{
    auto& s = self(p);
    std::string text = "LTPS";
    for (std::size_t i = 0; i < s.params.size(); ++i)
        text += " " + std::to_string(s.params[i].id) + "=" + std::to_string(s.values[i]);
    text += " patch=" + std::to_string(s.patch);
    const char* data = text.c_str();
    std::int64_t left = static_cast<std::int64_t>(text.size());
    while (left > 0)
    {
        const auto written = stream->write(stream, data, static_cast<std::uint64_t>(left));
        if (written <= 0)
            return false;
        data += written;
        left -= written;
    }
    return true;
}
bool stateLoad(const clap_plugin_t* p, const clap_istream_t* stream)
{
    auto& s = self(p);
    std::string text;
    char buffer[256];
    for (;;)
    {
        const auto n = stream->read(stream, buffer, sizeof(buffer));
        if (n < 0)
            return false;
        if (n == 0)
            break;
        text.append(buffer, static_cast<std::size_t>(n));
    }
    if (text.rfind("LTPS", 0) != 0)
        return false;
    std::size_t at = 4;
    while (at < text.size())
    {
        const auto space = text.find(' ', at);
        const auto next = text.find(' ', space + 1);
        const auto item = text.substr(space + 1, next == std::string::npos ? std::string::npos : next - space - 1);
        const auto eq = item.find('=');
        if (eq != std::string::npos)
        {
            const auto key = item.substr(0, eq);
            const double v = std::atof(item.substr(eq + 1).c_str());
            if (key == "patch")
                s.patch = static_cast<int>(v);
            else
                s.setParam(static_cast<clap_id>(std::atoi(key.c_str())), v);
        }
        if (next == std::string::npos)
            break;
        at = next;
    }
    return true;
}
const clap_plugin_state_t stateExt{stateSave, stateLoad};

// --- Latency --------------------------------------------------------------------------------------
std::uint32_t latencyGet(const clap_plugin_t* p) { return static_cast<std::uint32_t>(self(p).activeLatency); }
const clap_plugin_latency_t latencyExt{latencyGet};

// --- The window (the synth's): nothing in it, at a fixed size -------------------------------------
#if defined(_WIN32)
constexpr const char* guiApi = CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
constexpr const char* guiApi = CLAP_WINDOW_API_COCOA;
#else
constexpr const char* guiApi = CLAP_WINDOW_API_X11;
#endif
bool guiIsApiSupported(const clap_plugin_t*, const char* api, bool floating) { return !floating && std::strcmp(api, guiApi) == 0; }
bool guiGetPreferredApi(const clap_plugin_t*, const char** api, bool* floating)
{
    *api = guiApi;
    *floating = false;
    return true;
}
bool guiCreate(const clap_plugin_t* p, const char* api, bool floating)
{
    auto& s = self(p);
    if (s.guiCreated || floating || std::strcmp(api, guiApi) != 0)
        return false;
    s.guiCreated = true;
    return true;
}
void guiDestroy(const clap_plugin_t* p) { self(p).guiCreated = false; }
bool guiSetScale(const clap_plugin_t*, double) { return true; }
bool guiGetSize(const clap_plugin_t*, std::uint32_t* w, std::uint32_t* h)
{
    *w = 320;
    *h = 200;
    return true;
}
bool guiCanResize(const clap_plugin_t*) { return false; }
bool guiGetResizeHints(const clap_plugin_t*, clap_gui_resize_hints_t*) { return false; }
bool guiAdjustSize(const clap_plugin_t*, std::uint32_t*, std::uint32_t*) { return false; }
bool guiSetSize(const clap_plugin_t*, std::uint32_t, std::uint32_t) { return false; }
bool guiSetParent(const clap_plugin_t* p, const clap_window_t*) { return self(p).guiCreated; }
bool guiSetTransient(const clap_plugin_t*, const clap_window_t*) { return false; }
void guiSuggestTitle(const clap_plugin_t*, const char*) {}
bool guiShow(const clap_plugin_t* p) { return self(p).guiCreated; }
bool guiHide(const clap_plugin_t* p) { return self(p).guiCreated; }
const clap_plugin_gui_t guiExt{guiIsApiSupported, guiGetPreferredApi, guiCreate,      guiDestroy,        guiSetScale,
                               guiGetSize,        guiCanResize,       guiGetResizeHints, guiAdjustSize,     guiSetSize,
                               guiSetParent,      guiSetTransient,    guiSuggestTitle,   guiShow,           guiHide};

// --- The plugin ------------------------------------------------------------------------------------
bool pluginInit(const clap_plugin_t* p)
{
    auto& s = self(p);
    s.hostParams = static_cast<const clap_host_params_t*>(s.host->get_extension(s.host, CLAP_EXT_PARAMS));
    s.hostState = static_cast<const clap_host_state_t*>(s.host->get_extension(s.host, CLAP_EXT_STATE));
    s.hostLatency = static_cast<const clap_host_latency_t*>(s.host->get_extension(s.host, CLAP_EXT_LATENCY));
    return true;
}
void pluginDestroy(const clap_plugin_t* p) { delete &self(p); }
bool pluginActivate(const clap_plugin_t* p, double rate, std::uint32_t, std::uint32_t)
{
    auto& s = self(p);
    s.rate = rate;
    s.active = true;
    s.restartAsked = false;
    if (s.kind == Kind::latency)
    {
        s.activeLatency = static_cast<int>(s.value(1));
        for (auto& r : s.ring)
            r.assign(static_cast<std::size_t>(s.activeLatency) + 1, 0.0f);
        s.ringPos = 0;
    }
    for (auto& channel : s.voices)
        for (auto& v : channel)
            v = {};
    return true;
}
void pluginDeactivate(const clap_plugin_t* p) { self(p).active = false; }
bool pluginStartProcessing(const clap_plugin_t*) { return true; }
void pluginStopProcessing(const clap_plugin_t*) {}
void pluginReset(const clap_plugin_t* p)
{
    for (auto& channel : self(p).voices)
        for (auto& v : channel)
            v.on = false;
}

void handleNote(Plugin& s, const clap_event_header_t* h, const clap_output_events_t* out)
{
    if (h->space_id != CLAP_CORE_EVENT_SPACE_ID)
        return;
    int type = -1, channel = 0, key = 0;
    double velocity = 0.0;
    std::int32_t id = -1;
    if (h->type == CLAP_EVENT_NOTE_ON || h->type == CLAP_EVENT_NOTE_OFF)
    {
        const auto* e = reinterpret_cast<const clap_event_note_t*>(h);
        type = h->type == CLAP_EVENT_NOTE_ON ? 0x90 : 0x80;
        channel = e->channel < 0 ? 0 : e->channel & 15;
        key = e->key & 127;
        velocity = e->velocity;
        id = e->note_id;
    }
    else if (h->type == CLAP_EVENT_MIDI)
    {
        const auto* e = reinterpret_cast<const clap_event_midi_t*>(h);
        const int status = e->data[0] & 0xf0;
        channel = e->data[0] & 15;
        if (status == 0x90 || status == 0x80)
        {
            type = status == 0x90 && e->data[2] > 0 ? 0x90 : 0x80;
            key = e->data[1] & 127;
            velocity = e->data[2] / 127.0;
        }
        else if (s.kind == Kind::synth && status == 0xb0 && e->data[1] == 20)
        {
            // As if Level were turned in its window: a gesture, reported to the host.
            s.levelToSend = e->data[2] / 127.0;
            s.setParam(10, s.levelToSend);
            s.sendLevel = true;
        }
        else if (s.kind == Kind::synth && status == 0xc0)
        {
            s.patch = e->data[1];
            if (s.hostState != nullptr)
                s.hostState->mark_dirty(s.host); // on the audio thread; a host takes it from any thread
        }
        else if (s.kind == Kind::synth && status == 0xe0)
        {
            // Plain MIDI bend on a channel: tunes that channel's notes, over 48 semitones (MPE).
            const double bend = (((e->data[2] << 7) | e->data[1]) - 8192) / 8191.0 * 48.0;
            for (auto& v : s.voices[channel])
                v.tuning = bend;
        }
    }
    else if (h->type == CLAP_EVENT_NOTE_EXPRESSION && s.kind == Kind::synth)
    {
        const auto* e = reinterpret_cast<const clap_event_note_expression_t*>(h);
        if (e->expression_id == CLAP_NOTE_EXPRESSION_TUNING && e->key >= 0 && e->channel >= 0)
            s.voices[e->channel & 15][e->key & 127].tuning = e->value;
        return;
    }
    if (type < 0)
        return;

    if (s.kind == Kind::arp)
    {
        for (const int k : {key, key + 12})
        {
            if (k > 127)
                continue;
            clap_event_note_t n{};
            n.header = {sizeof(n), h->time, CLAP_CORE_EVENT_SPACE_ID,
                        static_cast<std::uint16_t>(type == 0x90 ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0};
            n.note_id = -1;
            n.port_index = 0;
            n.channel = static_cast<std::int16_t>(channel);
            n.key = static_cast<std::int16_t>(k);
            n.velocity = velocity;
            out->try_push(out, &n.header);
        }
        return;
    }
    if (s.kind != Kind::synth)
        return;
    auto& v = s.voices[channel][key];
    if (type == 0x90)
    {
        if (!v.on)
            v.phase = 0.0;
        v.on = true;
        v.key = key;
        v.channel = channel;
        v.id = id;
    }
    else
        v.on = false;
}

clap_process_status pluginProcess(const clap_plugin_t* p, const clap_process_t* process)
{
    auto& s = self(p);
    const auto n = process->frames_count;
    const auto* in = process->in_events;
    for (std::uint32_t i = 0; in != nullptr && i < in->size(in); ++i)
    {
        const auto* h = in->get(in, i);
        if (h->space_id == CLAP_CORE_EVENT_SPACE_ID && h->type == CLAP_EVENT_PARAM_VALUE)
        {
            const auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
            s.setParam(e->param_id, e->value);
        }
        else
            handleNote(s, h, process->out_events);
    }
    if (s.sendLevel)
    {
        s.sendLevel = false;
        const auto* out = process->out_events;
        clap_event_param_gesture_t g{};
        g.header = {sizeof(g), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_GESTURE_BEGIN, 0};
        g.param_id = 10;
        out->try_push(out, &g.header);
        clap_event_param_value_t v{};
        v.header = {sizeof(v), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
        v.param_id = 10;
        v.note_id = -1;
        v.port_index = -1;
        v.channel = -1;
        v.key = -1;
        v.value = s.levelToSend;
        out->try_push(out, &v.header);
        g.header.type = CLAP_EVENT_PARAM_GESTURE_END;
        out->try_push(out, &g.header);
    }

    switch (s.kind)
    {
    case Kind::synth:
    {
        float* l = process->audio_outputs[0].data32[0];
        float* r = process->audio_outputs[0].data32[1];
        std::fill(l, l + n, 0.0f);
        std::fill(r, r + n, 0.0f);
        const double level = s.value(10);
        const int wave = static_cast<int>(std::lround(s.value(20)));
        for (auto& channel : s.voices)
            for (auto& v : channel)
            {
                if (!v.on)
                    continue;
                const double hz = 440.0 * std::pow(2.0, (v.key - 69 + v.tuning) / 12.0);
                const double step = hz / s.rate;
                for (std::uint32_t i = 0; i < n; ++i)
                {
                    double x = std::sin(2.0 * 3.14159265358979323846 * v.phase);
                    if (wave == 1)
                        x = x >= 0.0 ? 1.0 : -1.0;
                    else if (wave == 2)
                        x = 2.0 * v.phase - 1.0;
                    const auto sample = static_cast<float>(level * x);
                    l[i] += sample;
                    r[i] += sample;
                    v.phase += step;
                    v.phase -= std::floor(v.phase);
                }
            }
        return CLAP_PROCESS_CONTINUE;
    }
    case Kind::arp:
        return CLAP_PROCESS_CONTINUE;
    case Kind::latency:
    {
        const auto size = s.ring[0].size();
        for (int c = 0; c < 2; ++c)
        {
            const float* x = process->audio_inputs[0].data32[c];
            float* y = process->audio_outputs[0].data32[c];
            std::size_t pos = s.ringPos;
            for (std::uint32_t i = 0; i < n; ++i)
            {
                s.ring[c][pos] = x[i];
                y[i] = s.ring[c][(pos + size - static_cast<std::size_t>(s.activeLatency)) % size];
                pos = (pos + 1) % size;
            }
        }
        s.ringPos = (s.ringPos + n) % size;
        return CLAP_PROCESS_CONTINUE;
    }
    case Kind::sidechain:
        for (int c = 0; c < 2; ++c)
        {
            const float* x = process->audio_inputs[0].data32[c];
            const float* side = process->audio_inputs[1].data32[c];
            float* y = process->audio_outputs[0].data32[c];
            for (std::uint32_t i = 0; i < n; ++i)
                y[i] = x[i] + side[i];
        }
        return CLAP_PROCESS_CONTINUE;
    case Kind::allocator:
    {
        auto garbage = std::make_unique<std::vector<float>>(64); // on the audio thread, on purpose
        for (int c = 0; c < 2; ++c)
            std::copy(process->audio_inputs[0].data32[c], process->audio_inputs[0].data32[c] + n,
                      process->audio_outputs[0].data32[c]);
        (*garbage)[0] = 1.0f;
        return CLAP_PROCESS_CONTINUE;
    }
    }
    return CLAP_PROCESS_CONTINUE;
}

const void* pluginExtension(const clap_plugin_t* p, const char* id)
{
    const auto kind = self(p).kind;
    if (std::strcmp(id, CLAP_EXT_AUDIO_PORTS) == 0)
        return &audioPorts;
    if (std::strcmp(id, CLAP_EXT_NOTE_PORTS) == 0 && (kind == Kind::synth || kind == Kind::arp))
        return &notePorts;
    if (std::strcmp(id, CLAP_EXT_PARAMS) == 0 && !self(p).params.empty())
        return &paramsExt;
    if (std::strcmp(id, CLAP_EXT_STATE) == 0)
        return &stateExt;
    if (std::strcmp(id, CLAP_EXT_LATENCY) == 0 && kind == Kind::latency)
        return &latencyExt;
    if (std::strcmp(id, CLAP_EXT_GUI) == 0 && kind == Kind::synth)
        return &guiExt;
    return nullptr;
}
void pluginOnMainThread(const clap_plugin_t*) {}

// --- The factory and the entry -------------------------------------------------------------------
std::uint32_t factoryCount(const clap_plugin_factory_t*) { return descriptorCount; }
const clap_plugin_descriptor_t* factoryDescriptor(const clap_plugin_factory_t*, std::uint32_t index)
{
    return index < descriptorCount ? &descriptors[index] : nullptr;
}
const clap_plugin_t* factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host, const char* id)
{
    for (std::uint32_t i = 0; i < descriptorCount; ++i)
    {
        if (std::strcmp(id, descriptors[i].id) != 0)
            continue;
        auto* s = new Plugin();
        s->host = host;
        s->kind = static_cast<Kind>(i);
        if (s->kind == Kind::synth)
            s->params.assign(std::begin(synthParams), std::end(synthParams));
        else if (s->kind == Kind::latency)
            s->params.assign(std::begin(latencyParams), std::end(latencyParams));
        for (const auto& param : s->params)
            s->values.push_back(param.def);
        if (s->kind == Kind::latency)
            s->activeLatency = static_cast<int>(s->value(1));
        s->clap = {&descriptors[i],      s,
                   pluginInit,           pluginDestroy,
                   pluginActivate,       pluginDeactivate,
                   pluginStartProcessing, pluginStopProcessing,
                   pluginReset,          pluginProcess,
                   pluginExtension,      pluginOnMainThread};
        return &s->clap;
    }
    return nullptr;
}
const clap_plugin_factory_t factory{factoryCount, factoryDescriptor, factoryCreate};

bool entryInit(const char*) { return true; }
void entryDeinit() {}
const void* entryFactory(const char* id) { return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) == 0 ? &factory : nullptr; }
} // namespace

LATTICE_EXPORT const clap_plugin_entry_t clap_entry{CLAP_VERSION_INIT, entryInit, entryDeinit, entryFactory};
