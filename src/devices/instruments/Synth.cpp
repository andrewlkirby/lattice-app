#include "devices/instruments/Synth.h"

#include "devices/dsp/OscillatorSimd.h"
#include "devices/dsp/SyncRates.h"
#include "devices/dsp/VoiceFilterSimd.h"

#include <algorithm>
#include <cmath>

namespace lattice
{
namespace
{
using P = SynthDevice::Param;

juce::String percent(float v)
{
    return juce::String(juce::roundToInt(v * 100.0f)) + " %";
}

juce::String signedPercent(float v)
{
    const int p = juce::roundToInt(v * 100.0f);
    return (p > 0 ? "+" : "") + juce::String(p) + " %";
}

juce::String octaves(float v)
{
    return (v > 0.0f ? "+" : "") + juce::String(v, 2) + " oct";
}

juce::String panText(float v)
{
    const int p = juce::roundToInt(std::abs(v) * 50.0f);
    return p == 0 ? juce::String("C") : juce::String(p) + (v < 0.0f ? "L" : "R");
}

ParameterSpec time(const char* id, const char* name, float def, float max = 20000.0f)
{
    return ParameterSpec::continuous(id, name, 0.5f, max, def, ParamUnit::Milliseconds).withCentre(200.0f);
}

ParameterSpec level01(const char* id, const char* name, float def)
{
    return ParameterSpec::continuous(id, name, 0.0f, 1.0f, def, ParamUnit::Percent).withFormatter(percent);
}

std::vector<ParameterSpec> makeSpecs()
{
    std::vector<ParameterSpec> s(static_cast<std::size_t>(P::numParams));
    const auto set = [&](int index, ParameterSpec spec, const char* group) {
        spec.group = group;
        s[static_cast<std::size_t>(index)] = std::move(spec);
    };
    const juce::StringArray shapes{"Sine", "Triangle", "Saw", "Pulse", "Wavetable"};
    const auto tables = dsp::WaveformLibrary::get().wavetableNames();

    for (int o = 0; o < 2; ++o)
    {
        const juce::String n(o + 1);
        const char* group = o == 0 ? "Osc 1" : "Osc 2";
        const int base = o == 0 ? P::osc1Shape : P::osc2Shape;
        const auto id = [&](const char* leaf) { return "osc" + n + "." + leaf; };
        set(base + 0, ParameterSpec::choice(id("shape"), "Shape", shapes, 2), group);
        set(base + 1,
            ParameterSpec::continuous(id("level"), "Level", -70.0f, 0.0f, o == 0 ? -3.0f : -8.0f, ParamUnit::Decibels)
                .withCentre(-12.0f),
            group);
        set(base + 2, ParameterSpec::integer(id("transpose"), "Transpose", -48, 48, 0, ParamUnit::TableSteps), group);
        set(base + 3,
            ParameterSpec::continuous(id("detune"), "Detune", -100.0f, 100.0f, o == 0 ? 0.0f : 7.0f, ParamUnit::Cents),
            group);
        set(base + 4,
            ParameterSpec::continuous(id("pw"), "Pulse Width", 0.02f, 0.98f, 0.5f, ParamUnit::Percent)
                .withFormatter(percent),
            group);
        set(base + 5, ParameterSpec::choice(id("table"), "Wavetable", tables, 0), group);
        set(base + 6, level01(id("position").toRawUTF8(), "Position", 0.0f), group);
    }
    set(P::osc2Sync, ParameterSpec::toggle("osc2.sync", "Sync", false), "Osc 2");

    set(P::subLevel, ParameterSpec::continuous("sub.level", "Sub", -70.0f, 0.0f, -70.0f, ParamUnit::Decibels).withCentre(-12.0f),
        "Sub / Noise");
    set(P::subOctave, ParameterSpec::choice("sub.octave", "Sub Octave", {"-1 oct", "-2 oct"}, 0), "Sub / Noise");
    set(P::noiseLevel,
        ParameterSpec::continuous("noise.level", "Noise", -70.0f, 0.0f, -70.0f, ParamUnit::Decibels).withCentre(-12.0f),
        "Sub / Noise");
    set(P::noiseColour, level01("noise.colour", "Noise Colour", 1.0f), "Sub / Noise");

    set(P::unisonVoices, ParameterSpec::integer("unison.voices", "Unison", 1, SynthDevice::maxUnison, 1), "Unison");
    set(P::unisonDetune,
        ParameterSpec::continuous("unison.detune", "Unison Detune", 0.0f, 100.0f, 18.0f, ParamUnit::Cents), "Unison");
    set(P::unisonSpread, level01("unison.spread", "Unison Spread", 0.7f), "Unison");

    set(P::filterType,
        ParameterSpec::choice("filter.type", "Filter Type",
                              {"Low-pass 12", "Low-pass 24", "High-pass 12", "High-pass 24", "Band-pass", "Notch",
                               "Ladder"},
                              1),
        "Filter");
    set(P::filterCutoff,
        ParameterSpec::continuous("filter.cutoff", "Cutoff", 20.0f, 20000.0f, 3000.0f, ParamUnit::Hz).withCentre(1000.0f),
        "Filter");
    set(P::filterResonance, level01("filter.resonance", "Resonance", 0.15f), "Filter");
    set(P::filterDrive, ParameterSpec::continuous("filter.drive", "Drive", 0.0f, 24.0f, 0.0f, ParamUnit::Decibels),
        "Filter");
    set(P::filterKeytrack, level01("filter.keytrack", "Key Track", 0.5f), "Filter");
    set(P::filterEnvAmount,
        ParameterSpec::continuous("filter.envAmount", "Env 2 Amount", -6.0f, 6.0f, 0.0f).withFormatter(octaves),
        "Filter");
    set(P::filterVelocity,
        ParameterSpec::continuous("filter.velocity", "Velocity", 0.0f, 4.0f, 0.0f).withFormatter(octaves), "Filter");

    set(P::ampAttack, time("amp.attack", "Attack", 2.0f), "Amp Env");
    set(P::ampDecay, time("amp.decay", "Decay", 600.0f), "Amp Env");
    set(P::ampSustain, level01("amp.sustain", "Sustain", 0.7f), "Amp Env");
    set(P::ampRelease, time("amp.release", "Release", 250.0f), "Amp Env");
    set(P::ampVelocity, level01("amp.velocity", "Velocity", 0.5f), "Amp Env");

    set(P::env2Attack, time("env2.attack", "Attack", 2.0f), "Env 2");
    set(P::env2Decay, time("env2.decay", "Decay", 400.0f), "Env 2");
    set(P::env2Sustain, level01("env2.sustain", "Sustain", 0.0f), "Env 2");
    set(P::env2Release, time("env2.release", "Release", 300.0f), "Env 2");

    const juce::StringArray lfoShapes{"Sine", "Triangle", "Saw Up", "Saw Down", "Square", "S&H"};
    for (int l = 0; l < 2; ++l)
    {
        const juce::String n(l + 1);
        const char* group = l == 0 ? "LFO 1" : "LFO 2";
        const int base = l == 0 ? P::lfo1Shape : P::lfo2Shape;
        set(base + 0, ParameterSpec::choice("lfo" + n + ".shape", "Shape", lfoShapes, 0), group);
        set(base + 1,
            ParameterSpec::continuous("lfo" + n + ".rate", "Rate", 0.01f, 40.0f, 4.0f, ParamUnit::Hz).withCentre(2.0f),
            group);
        set(base + 2, ParameterSpec::toggle("lfo" + n + ".sync", "Sync", false), group);
        set(base + 3, ParameterSpec::choice("lfo" + n + ".syncRate", "Synced Rate", dsp::SyncRates::names(),
                                            dsp::SyncRates::indexOf("1/4")),
            group);
        set(base + 4, ParameterSpec::toggle("lfo" + n + ".retrigger", "Retrigger", true), group);
    }

    const juce::StringArray sources{"Off", "Env 2", "LFO 1", "LFO 2", "Velocity", "Aftertouch",
                                    "Mod Wheel", "Key", "Amp Env", "Random"};
    const juce::StringArray dests{"Off",        "Pitch",      "Osc 1 Pitch", "Osc 2 Pitch", "Osc 1 PW",   "Osc 2 PW",
                                  "Osc 1 Pos",  "Osc 2 Pos",  "Osc 1 Level", "Osc 2 Level", "Cutoff",     "Resonance",
                                  "Pan",        "Amp",        "LFO 1 Rate",  "LFO 2 Rate",  "Noise Level", "Sub Level"};
    for (int m = 0; m < SynthDevice::numModSlots; ++m)
    {
        const juce::String n(m + 1);
        const int base = P::mod1Source + 3 * m;
        set(base + 0, ParameterSpec::choice("mod" + n + ".source", "Source " + n, sources, 0), "Mod Matrix");
        set(base + 1, ParameterSpec::choice("mod" + n + ".dest", "Dest " + n, dests, 0), "Mod Matrix");
        set(base + 2,
            ParameterSpec::continuous("mod" + n + ".amount", "Amount " + n, -1.0f, 1.0f, 0.0f).withFormatter(signedPercent),
            "Mod Matrix");
    }

    set(P::voiceMode, ParameterSpec::choice("voice.mode", "Mode", {"Poly", "Mono", "Legato"}, 0), "Voice");
    set(P::polyphony, ParameterSpec::integer("voice.polyphony", "Voices", 1, SynthDevice::maxPolyphony, 16), "Voice");
    set(P::glideTime, ParameterSpec::continuous("voice.glide", "Glide", 0.0f, 5000.0f, 0.0f, ParamUnit::Milliseconds)
                          .withCentre(300.0f),
        "Voice");

    set(P::outVolume,
        ParameterSpec::continuous("out.volume", "Volume", -70.0f, 6.0f, -6.0f, ParamUnit::Decibels).withCentre(-12.0f),
        "Output");
    set(P::outPan, ParameterSpec::continuous("out.pan", "Pan", -1.0f, 1.0f, 0.0f).withFormatter(panText), "Output");
    return s;
}

float dbToGain(float db)
{
    return db <= -69.95f ? 0.0f : std::pow(10.0f, db / 20.0f);
}

/** Constant-power pan, -1..1. */
void panGains(float pan, float& left, float& right)
{
    const float angle = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * 0.25f * 3.14159265f;
    left = std::cos(angle);
    right = std::sin(angle);
}

constexpr double referenceHz = 261.6255653; // key tracking is relative to middle C's frequency
} // namespace

SynthDevice::SynthDevice(const TrackTuning& t)
    : StudioDevice(typeId, "Synth", DeviceKind::Instrument, makeSpecs()), tuning(t), waves(dsp::WaveformLibrary::get())
{
}

void SynthDevice::prepare(double sr, int)
{
    for (auto& v : voices)
    {
        v.amp.setSampleRate(sr);
        v.env2.setSampleRate(sr);
    }
    gainStep = static_cast<float>(1.0 / (0.02 * sr));
    reset();
}

void SynthDevice::reset() noexcept
{
    for (auto& v : voices)
    {
        v.amp.reset();
        v.env2.reset();
        v.note = -1;
        v.gate = v.keyDown = v.stealing = false;
        v.svf[0].reset();
        v.svf[1].reset();
        v.ladder[0].reset();
        v.ladder[1].reset();
        v.filterG = -1.0f;
        v.started = false;
    }
    monoCount = 0;
    sustainDown = false;
    bend = 0.0f;
    lastPitch = -1.0;
}

void SynthDevice::releaseAllNotes() noexcept
{
    for (auto& v : voices)
    {
        if (v.isActive() && !v.stealing)
        {
            v.amp.noteOff();
            v.env2.noteOff();
        }
        v.gate = v.keyDown = false;
    }
    monoCount = 0;
    sustainDown = false;
}

void SynthDevice::choke() noexcept
{
    for (auto& v : voices)
        if (v.isActive() && !v.stealing)
        {
            v.stealing = true;
            v.gate = v.keyDown = false;
            v.amp.kill(static_cast<float>(stealFadeSeconds));
        }
    monoCount = 0;
}

int SynthDevice::activeVoiceCount() const noexcept
{
    int n = 0;
    for (const auto& v : voices)
        if (v.isActive())
            ++n;
    return n;
}

void SynthDevice::readPatch() noexcept
{
    auto& p = patch;
    for (int o = 0; o < 2; ++o)
    {
        const int base = o == 0 ? P::osc1Shape : P::osc2Shape;
        p.shape[o] = static_cast<Shape>(juce::roundToInt(value(base + 0)));
        p.level[o] = dbToGain(value(base + 1));
        p.transpose[o] = value(base + 2);
        p.detune[o] = value(base + 3);
        p.pw[o] = value(base + 4);
        p.table[o] = juce::jlimit(0, waves.numWavetables() - 1, juce::roundToInt(value(base + 5)));
        p.position[o] = value(base + 6);
    }
    p.sync = value(P::osc2Sync) >= 0.5f;
    p.sub = dbToGain(value(P::subLevel));
    p.subOctave = juce::roundToInt(value(P::subOctave)) + 1;
    p.noise = dbToGain(value(P::noiseLevel));
    p.noiseColour = value(P::noiseColour);

    p.unison = juce::jlimit(1, maxUnison, juce::roundToInt(value(P::unisonVoices)));
    p.unisonDetune = value(P::unisonDetune);
    p.unisonSpread = value(P::unisonSpread);
    for (int u = 0; u < maxUnison; ++u)
    {
        const float x = p.unison > 1 ? 2.0f * static_cast<float>(u) / static_cast<float>(p.unison - 1) - 1.0f : 0.0f;
        p.unisonRatio[static_cast<std::size_t>(u)] = std::exp2(static_cast<double>(x * p.unisonDetune) / 1200.0);
        panGains(x * p.unisonSpread, p.unisonLeft[static_cast<std::size_t>(u)], p.unisonRight[static_cast<std::size_t>(u)]);
    }
    p.unisonNormal = 1.0f / std::sqrt(static_cast<float>(p.unison));

    p.filter = static_cast<FilterType>(juce::roundToInt(value(P::filterType)));
    p.cutoff = value(P::filterCutoff);
    p.resonance = value(P::filterResonance);
    p.drive = std::pow(10.0f, value(P::filterDrive) / 20.0f);
    p.keytrack = value(P::filterKeytrack);
    p.envAmount = value(P::filterEnvAmount);
    p.filterVelocity = value(P::filterVelocity);

    p.ampEnv.attackSeconds = value(P::ampAttack) * 0.001f;
    p.ampEnv.decaySeconds = value(P::ampDecay) * 0.001f;
    p.ampEnv.sustain = value(P::ampSustain);
    p.ampEnv.releaseSeconds = value(P::ampRelease) * 0.001f;
    p.ampVelocity = value(P::ampVelocity);
    p.modEnv.attackSeconds = value(P::env2Attack) * 0.001f;
    p.modEnv.decaySeconds = value(P::env2Decay) * 0.001f;
    p.modEnv.sustain = value(P::env2Sustain);
    p.modEnv.releaseSeconds = value(P::env2Release) * 0.001f;

    for (int l = 0; l < 2; ++l)
    {
        const int base = l == 0 ? P::lfo1Shape : P::lfo2Shape;
        p.lfoShape[l] = static_cast<dsp::LfoShape>(juce::roundToInt(value(base + 0)));
        p.lfoRate[l] = value(base + 1);
        p.lfoSync[l] = value(base + 2) >= 0.5f;
        p.lfoBeats[l] = dsp::SyncRates::beatsAt(juce::roundToInt(value(base + 3)));
        p.lfoRetrigger[l] = value(base + 4) >= 0.5f;
    }
    for (int m = 0; m < numModSlots; ++m)
    {
        p.modSource[m] = static_cast<ModSource>(juce::roundToInt(value(P::mod1Source + 3 * m)));
        p.modDest[m] = static_cast<ModDest>(juce::roundToInt(value(P::mod1Dest + 3 * m)));
        p.modAmount[m] = value(P::mod1Amount + 3 * m);
    }
    p.mode = static_cast<VoiceMode>(juce::roundToInt(value(P::voiceMode)));
    p.polyphony = juce::jlimit(1, maxPolyphony, juce::roundToInt(value(P::polyphony)));
    p.glideSeconds = value(P::glideTime) * 0.001;

    float l = 0.0f;
    float r = 0.0f;
    panGains(value(P::outPan), l, r);
    const float g = dbToGain(value(P::outVolume)) * 1.41421356f; // centre pan is unity
    targetGainLeft = g * l;
    targetGainRight = g * r;

    for (auto& v : voices)
    {
        v.amp.setSettings(p.ampEnv);
        v.env2.setSettings(p.modEnv);
    }
}

// --- Notes -----------------------------------------------------------------------------------------------

SynthDevice::Voice* SynthDevice::allocateVoice() noexcept
{
    int sounding = 0;
    Voice* idle = nullptr;
    for (auto& v : voices)
    {
        if (!v.isActive())
        {
            if (idle == nullptr)
                idle = &v;
        }
        else if (!v.stealing)
        {
            ++sounding;
        }
    }

    if (sounding >= patch.polyphony)
    {
        // Steal: the oldest released voice, else the oldest.
        Voice* victim = nullptr;
        for (auto& v : voices)
        {
            if (!v.isActive() || v.stealing)
                continue;
            if (victim == nullptr || (!v.gate && victim->gate) || (v.gate == victim->gate && v.age < victim->age))
                victim = &v;
        }
        if (victim != nullptr)
        {
            victim->stealing = true;
            victim->gate = victim->keyDown = false;
            victim->amp.kill(static_cast<float>(stealFadeSeconds));
        }
    }

    if (idle != nullptr)
        return idle;
    // Every slot busy (spares still fading): cut the oldest fading voice.
    Voice* oldest = nullptr;
    for (auto& v : voices)
        if (oldest == nullptr || v.age < oldest->age)
            oldest = &v;
    return oldest;
}

void SynthDevice::startVoice(Voice& v, int note, float velocity, int channel, bool retrigger) noexcept
{
    const bool fresh = !v.isActive() || v.stealing;
    v.note = note;
    v.channel = channel;
    v.velocity = velocity;
    v.gate = v.keyDown = true;
    v.stealing = false;
    v.age = ++noteCounter;
    v.pitchTarget = note;

    const double from = fresh ? (lastPitch >= 0.0 ? lastPitch : note) : v.pitch;
    if (patch.glideSeconds > 0.0 && from != note && (!fresh || lastPitch >= 0.0))
    {
        v.pitch = from;
        v.glideRate = std::abs(note - from) / (patch.glideSeconds * sampleRate());
    }
    else
    {
        v.pitch = note;
        v.glideRate = 0.0;
    }
    lastPitch = note;

    if (fresh)
    {
        // Oscillator phases: the first copy starts at 0 (a consistent attack), the others spread out.
        std::uint32_t seed = randomSeed;
        for (int u = 0; u < maxUnison; ++u)
        {
            seed = seed * 1664525u + 1013904223u;
            v.osc1[static_cast<std::size_t>(u)].phase = u == 0 ? 0u : seed;
            seed = seed * 1664525u + 1013904223u;
            v.osc2[static_cast<std::size_t>(u)].phase = u == 0 ? 0u : seed;
            v.osc2[static_cast<std::size_t>(u)].blep = 0.0f;
        }
        randomSeed = seed;
        v.subPhase = 0;
        v.svf[0].reset();
        v.svf[1].reset();
        v.ladder[0].reset();
        v.ladder[1].reset();
        v.filterG = -1.0f;
        v.started = false;
        v.noiseSeed = seed | 1u;
        v.pressure = 0.0f;
    }
    randomSeed = randomSeed * 1664525u + 1013904223u;
    v.random = static_cast<float>(randomSeed >> 8) * (2.0f / 16777216.0f) - 1.0f;

    if (retrigger || fresh)
    {
        v.amp.noteOn();
        v.env2.noteOn();
        for (int l = 0; l < 2; ++l)
        {
            auto& lfo = l == 0 ? v.lfo1 : v.lfo2;
            lfo.seed(randomSeed + static_cast<std::uint32_t>(l));
            lfo.reset(patch.lfoRetrigger[l] ? 0.0 : freeLfoPhase[l]);
        }
    }
}

int SynthDevice::heldNoteForMono() const noexcept
{
    return monoCount > 0 ? monoStack[static_cast<std::size_t>(monoCount - 1)] : -1;
}

void SynthDevice::noteOn(int note, int velocity, int channel) noexcept
{
    const float vel = static_cast<float>(velocity) / 127.0f;
    if (patch.mode == VoiceMode::Poly)
    {
        if (auto* v = allocateVoice())
            startVoice(*v, note, vel, channel, true);
        return;
    }

    // Mono and legato: one voice, the last key pressed wins.
    const bool othersHeld = monoCount > 0;
    for (int i = 0; i < monoCount; ++i)
        if (monoStack[static_cast<std::size_t>(i)] == note)
        {
            std::copy(monoStack.begin() + i + 1, monoStack.begin() + monoCount, monoStack.begin() + i);
            --monoCount;
            break;
        }
    if (monoCount < static_cast<int>(monoStack.size()))
        monoStack[static_cast<std::size_t>(monoCount++)] = static_cast<std::int16_t>(note);

    auto& v = voices[0];
    for (std::size_t i = 1; i < voices.size(); ++i) // anything playing in other slots ends
        if (voices[i].isActive() && !voices[i].stealing)
        {
            voices[i].stealing = true;
            voices[i].amp.kill(static_cast<float>(stealFadeSeconds));
        }
    const bool legato = patch.mode == VoiceMode::Legato && othersHeld && v.isActive() && !v.stealing;
    if (v.isActive() && !v.stealing)
    {
        // Glide from where the voice is now.
        lastPitch = v.pitch;
    }
    startVoice(v, note, vel, channel, !legato);
}

void SynthDevice::noteOff(int note, int channel) noexcept
{
    if (patch.mode != VoiceMode::Poly)
    {
        const bool wasCurrent = heldNoteForMono() == note;
        for (int i = 0; i < monoCount; ++i)
            if (monoStack[static_cast<std::size_t>(i)] == note)
            {
                std::copy(monoStack.begin() + i + 1, monoStack.begin() + monoCount, monoStack.begin() + i);
                --monoCount;
                break;
            }
        auto& v = voices[0];
        if (!wasCurrent || !v.isActive())
            return;
        if (monoCount > 0)
        {
            // Back to the key still held, gliding, without retriggering.
            lastPitch = v.pitch;
            startVoice(v, heldNoteForMono(), v.velocity, channel, false);
            return;
        }
        v.keyDown = false;
        if (!sustainDown)
        {
            v.gate = false;
            v.amp.noteOff();
            v.env2.noteOff();
        }
        return;
    }

    for (auto& v : voices)
    {
        if (v.note != note || !v.keyDown || v.channel != channel || v.stealing)
            continue;
        v.keyDown = false;
        if (!sustainDown)
        {
            v.gate = false;
            v.amp.noteOff();
            v.env2.noteOff();
        }
    }
}

void SynthDevice::setSustain(bool down) noexcept
{
    sustainDown = down;
    if (down)
        return;
    for (auto& v : voices)
        if (v.isActive() && v.gate && !v.keyDown && !v.stealing)
        {
            v.gate = false;
            v.amp.noteOff();
            v.env2.noteOff();
        }
}

void SynthDevice::handleEvent(const TimedMidi& e, const DeviceTimeInfo&) noexcept
{
    const int status = e.bytes[0] & 0xf0;
    const int channel = e.bytes[0] & 0x0f;
    switch (status)
    {
    case 0x90:
        if (e.bytes[2] > 0)
            noteOn(e.bytes[1], e.bytes[2], channel);
        else
            noteOff(e.bytes[1], channel);
        break;
    case 0x80:
        noteOff(e.bytes[1], channel);
        break;
    case 0xe0:
        bend = std::clamp(static_cast<float>(((e.bytes[2] << 7) | e.bytes[1]) - 8192) / 8191.0f, -1.0f, 1.0f);
        break;
    case 0xd0:
        channelPressure = static_cast<float>(e.bytes[1]) / 127.0f;
        break;
    case 0xa0:
        for (auto& v : voices)
            if (v.note == e.bytes[1] && v.isActive())
                v.pressure = static_cast<float>(e.bytes[2]) / 127.0f;
        break;
    case 0xb0:
        if (e.bytes[1] == 1)
            modWheel = static_cast<float>(e.bytes[2]) / 127.0f;
        else if (e.bytes[1] == 64)
            setSustain(e.bytes[2] >= 64);
        else if (e.bytes[1] == 120)
            reset();
        else if (e.bytes[1] == 123)
            releaseAllNotes();
        break;
    default:
        break;
    }
}

// --- Rendering -------------------------------------------------------------------------------------------

void SynthDevice::process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept
{
    readPatch();
    const int n = io.numSamples;
    float* left = io.channels[0];
    float* right = io.channels[io.numChannels > 1 ? 1 : 0];
    std::fill_n(left, n, 0.0f);
    if (right != left)
        std::fill_n(right, n, 0.0f);

    int pos = 0;
    for (const auto& e : *io.midiIn)
    {
        const int at = std::clamp(e.sampleOffset, 0, n);
        if (at > pos)
        {
            renderSegment(left + pos, right + pos, at - pos, time, pos);
            pos = at;
        }
        handleEvent(e, time);
    }
    if (pos < n)
        renderSegment(left + pos, right + pos, n - pos, time, pos);

    // Output level and pan, smoothed.
    for (int i = 0; i < n; ++i)
    {
        outGainLeft += std::clamp(targetGainLeft - outGainLeft, -gainStep, gainStep);
        outGainRight += std::clamp(targetGainRight - outGainRight, -gainStep, gainStep);
        const float mono = left[i];
        if (right != left)
        {
            left[i] *= outGainLeft;
            right[i] *= outGainRight;
        }
        else
        {
            left[i] = mono * 0.5f * (outGainLeft + outGainRight);
        }
    }

    for (int l = 0; l < 2; ++l)
    {
        const double cycles = patch.lfoRate[l] * n / sampleRate();
        freeLfoPhase[l] = freeLfoPhase[l] + cycles - std::floor(freeLfoPhase[l] + cycles);
    }
}

void SynthDevice::renderSegment(float* left, float* right, int numSamples, const DeviceTimeInfo& time,
                                int offset) noexcept
{
    const bool ladder = patch.filter == FilterType::Ladder;
    const bool steep = patch.filter == FilterType::LowPass24 || patch.filter == FilterType::HighPass24;
    dsp::SvfMode svfMode = dsp::SvfMode::LowPass;
    if (patch.filter == FilterType::HighPass12 || patch.filter == FilterType::HighPass24)
        svfMode = dsp::SvfMode::HighPass;
    else if (patch.filter == FilterType::BandPass)
        svfMode = dsp::SvfMode::BandPass;
    else if (patch.filter == FilterType::Notch)
        svfMode = dsp::SvfMode::Notch;

    // Each control block in three stages: every voice's oscillators, then all the voice filters
    // side by side (four per SIMD register), then every voice's amplitude and pan.
    for (int start = 0; start < numSamples; start += controlBlock)
    {
        const int m = std::min(controlBlock, numSamples - start);
        std::array<Voice*, voiceSlots> active{};
        int numActive = 0;
        for (auto& v : voices)
            if (v.isActive())
                active[static_cast<std::size_t>(numActive++)] = &v;
        if (numActive == 0)
            return;

        std::array<dsp::FilterLane, voiceSlots * 2> lanes{};
        int numLanes = 0;
        for (int i = 0; i < numActive; ++i)
        {
            auto& v = *active[static_cast<std::size_t>(i)];
            renderVoiceBlock(v, start, m, time, offset);
            for (int ch = 0; ch < (v.blockStereo ? 2 : 1); ++ch)
            {
                auto& lane = lanes[static_cast<std::size_t>(numLanes++)];
                lane.buffer = ch == 0 ? v.bufL.data() : v.bufR.data();
                lane.svf = &v.svf[ch];
                lane.ladder = &v.ladder[ch];
                lane.g = v.blockG;
                lane.gStep = v.blockGStep;
                lane.resonance = v.blockResonance;
            }
        }

        if (ladder)
            dsp::VoiceFilterSimd::ladder(lanes.data(), numLanes, m, patch.drive);
        else
            dsp::VoiceFilterSimd::svf(lanes.data(), numLanes, m, steep, svfMode, patch.drive);

        for (int i = 0; i < numActive; ++i)
            finishVoiceBlock(*active[static_cast<std::size_t>(i)], left + start, right + start, m);
    }

    for (auto& v : voices)
        if (!v.isActive() && v.note >= 0)
        {
            v.note = -1;
            v.stealing = false;
            v.gate = v.keyDown = false;
        }
}

void SynthDevice::renderVoiceBlock(Voice& v, int start, int m, const DeviceTimeInfo& time, int offset) noexcept
{
    const auto& p = patch;
    const double sr = sampleRate();
    const int unison = p.unison;
    const bool stereo = unison > 1 && p.unisonSpread > 0.0f;
    const bool ladder = p.filter == FilterType::Ladder;
    const bool steep = p.filter == FilterType::LowPass24 || p.filter == FilterType::HighPass24;
    dsp::SvfMode svfMode = dsp::SvfMode::LowPass;
    if (p.filter == FilterType::HighPass12 || p.filter == FilterType::HighPass24)
        svfMode = dsp::SvfMode::HighPass;
    else if (p.filter == FilterType::BandPass)
        svfMode = dsp::SvfMode::BandPass;
    else if (p.filter == FilterType::Notch)
        svfMode = dsp::SvfMode::Notch;

    {

        // --- Control rate: modulation sources -------------------------------------------------------
        const float env2Value = v.env2.advance(m);
        float lfoValue[2];
        float modSum[static_cast<int>(ModDest::count)]{};
        // LFO rates may themselves be modulated (by the previous block's sums would be circular:
        // the rate modulation uses this block's other sources only).
        float rateMod[2] = {0.0f, 0.0f};
        for (int s = 0; s < numModSlots; ++s)
        {
            if (p.modDest[s] == ModDest::Lfo1Rate || p.modDest[s] == ModDest::Lfo2Rate)
            {
                float src = 0.0f;
                switch (p.modSource[s])
                {
                case ModSource::Env2: src = env2Value; break;
                case ModSource::Velocity: src = v.velocity; break;
                case ModSource::Aftertouch: src = std::max(channelPressure, v.pressure); break;
                case ModSource::ModWheel: src = modWheel; break;
                case ModSource::Key: src = static_cast<float>((v.pitch - 60.0) / 48.0); break;
                case ModSource::AmpEnv: src = v.amp.currentValue(); break;
                case ModSource::Random: src = v.random; break;
                default: break;
                }
                rateMod[p.modDest[s] == ModDest::Lfo1Rate ? 0 : 1] += src * p.modAmount[s];
            }
        }
        for (int l = 0; l < 2; ++l)
        {
            auto& lfo = l == 0 ? v.lfo1 : v.lfo2;
            if (p.lfoSync[l])
            {
                const double beats = p.lfoBeats[l] / std::exp2(4.0 * rateMod[l]);
                lfo.setPhase(dsp::Lfo::phaseFromBeat(time.beatAt(offset + start), beats));
                lfoValue[l] = lfo.valueAt(p.lfoShape[l], lfo.currentPhase());
            }
            else
            {
                const double rate = p.lfoRate[l] * std::exp2(4.0 * rateMod[l]);
                lfoValue[l] = lfo.advance(p.lfoShape[l], rate * m / sr);
            }
        }
        for (int s = 0; s < numModSlots; ++s)
        {
            if (p.modDest[s] == ModDest::Off || p.modSource[s] == ModSource::Off)
                continue;
            float src = 0.0f;
            switch (p.modSource[s])
            {
            case ModSource::Off: break;
            case ModSource::Env2: src = env2Value; break;
            case ModSource::Lfo1: src = lfoValue[0]; break;
            case ModSource::Lfo2: src = lfoValue[1]; break;
            case ModSource::Velocity: src = v.velocity; break;
            case ModSource::Aftertouch: src = std::max(channelPressure, v.pressure); break;
            case ModSource::ModWheel: src = modWheel; break;
            case ModSource::Key: src = static_cast<float>((v.pitch - 60.0) / 48.0); break;
            case ModSource::AmpEnv: src = v.amp.currentValue(); break;
            case ModSource::Random: src = v.random; break;
            }
            modSum[static_cast<int>(p.modDest[s])] += src * p.modAmount[s];
        }
        const auto mod = [&](ModDest d) { return modSum[static_cast<int>(d)]; };

        // --- Pitch -----------------------------------------------------------------------------------
        if (v.glideRate > 0.0 && v.pitch != v.pitchTarget)
        {
            const double step = v.glideRate * m;
            if (std::abs(v.pitchTarget - v.pitch) <= step)
                v.pitch = v.pitchTarget;
            else
                v.pitch += v.pitch < v.pitchTarget ? step : -step;
        }
        double bendSteps = 0.0;
        double bendCents = 0.0;
        tuning.bendOffsets(bend, bendSteps, bendCents);
        const double basePitch = v.pitch + bendSteps + 24.0 * mod(ModDest::Pitch);

        std::uint32_t targetInc[2][maxUnison]{};
        dsp::MipmappedWave::Position at[2][maxUnison]{};
        double oscHz[2] = {0.0, 0.0};
        for (int o = 0; o < 2; ++o)
        {
            const double pitch = basePitch + p.transpose[o] + 24.0 * mod(o == 0 ? ModDest::Osc1Pitch : ModDest::Osc2Pitch);
            const double hz = tuning.hzAt(pitch, p.detune[o] + bendCents);
            oscHz[o] = hz;
            for (int u = 0; u < unison; ++u)
            {
                const auto inc = dsp::incrementFor(hz * p.unisonRatio[static_cast<std::size_t>(u)], sr);
                targetInc[o][u] = inc;
                at[o][u] = dsp::MipmappedWave::positionFor(inc);
            }
        }
        const std::uint32_t subTarget = dsp::incrementFor(oscHz[0] / (p.subOctave == 1 ? 2.0 : 4.0), sr);
        const auto subAt = dsp::MipmappedWave::positionFor(subTarget);
        if (!v.started)
        {
            for (int u = 0; u < unison; ++u)
            {
                v.osc1[static_cast<std::size_t>(u)].inc = targetInc[0][u];
                v.osc2[static_cast<std::size_t>(u)].inc = targetInc[1][u];
            }
            v.subInc = subTarget;
        }

        // Per-sample increment steps (interpolated across the block, so pitch moves smoothly).
        std::int64_t incStep[2][maxUnison]{};
        for (int o = 0; o < 2; ++o)
            for (int u = 0; u < unison; ++u)
            {
                const auto& copy = (o == 0 ? v.osc1 : v.osc2)[static_cast<std::size_t>(u)];
                incStep[o][u] = (static_cast<std::int64_t>(targetInc[o][u]) - static_cast<std::int64_t>(copy.inc)) / m;
            }
        const std::int64_t subStep = (static_cast<std::int64_t>(subTarget) - static_cast<std::int64_t>(v.subInc)) / m;

        // --- Filter ----------------------------------------------------------------------------------
        const double noteHz = oscHz[0] > 0.0 ? oscHz[0] : tuning.hzAt(v.pitch);
        const double keyFactor = noteHz > 0.0 ? std::pow(noteHz / referenceHz, static_cast<double>(p.keytrack)) : 1.0;
        const double octavesUp = p.envAmount * env2Value + p.filterVelocity * v.velocity + 6.0 * mod(ModDest::Cutoff);
        const double cutoff = std::clamp(p.cutoff * keyFactor * std::exp2(octavesUp), 20.0, 0.45 * sr);
        const float resonance = std::clamp(p.resonance + mod(ModDest::Resonance), 0.0f, 1.0f);
        const float gTarget = ladder ? dsp::LadderFilter::prewarpOversampled(cutoff, sr)
                                     : dsp::SvfCoefficients::prewarp(cutoff, sr);
        if (v.filterG < 0.0f)
            v.filterG = gTarget;
        const float gStep = (gTarget - v.filterG) / static_cast<float>(m);

        // --- Levels ----------------------------------------------------------------------------------
        const float level1 = p.level[0] * std::max(0.0f, 1.0f + mod(ModDest::Osc1Level)) * p.unisonNormal;
        const float level2 = p.level[1] * std::max(0.0f, 1.0f + mod(ModDest::Osc2Level)) * p.unisonNormal;
        const float subLevel = p.sub * std::max(0.0f, 1.0f + mod(ModDest::SubLevel));
        const float noiseLevel = p.noise * std::max(0.0f, 1.0f + mod(ModDest::NoiseLevel));
        float pw[2];
        float position[2];
        for (int o = 0; o < 2; ++o)
        {
            pw[o] = std::clamp(p.pw[o] + 0.5f * mod(o == 0 ? ModDest::Osc1Pw : ModDest::Osc2Pw), 0.02f, 0.98f);
            position[o] = std::clamp(p.position[o] + mod(o == 0 ? ModDest::Osc1Position : ModDest::Osc2Position), 0.0f, 1.0f);
        }
        const auto pwOffset = [](float w) { return static_cast<std::uint32_t>(static_cast<double>(w) * 4294967296.0); };
        const std::uint32_t pwOff[2] = {pwOffset(pw[0]), pwOffset(pw[1])};
        const float velocityGain = (1.0f - p.ampVelocity) + p.ampVelocity * v.velocity * v.velocity;
        const float ampMod = std::max(0.0f, 1.0f + mod(ModDest::Amp));
        const float panTarget = std::clamp(mod(ModDest::Pan), -1.0f, 1.0f);
        if (!v.started)
            v.lastPan = panTarget;
        float panFromL = 0.0f, panFromR = 0.0f, panToL = 0.0f, panToR = 0.0f;
        panGains(v.lastPan, panFromL, panFromR);
        panGains(panTarget, panToL, panToR);
        const float panStepL = (panToL - panFromL) / static_cast<float>(m);
        const float panStepR = (panToR - panFromR) / static_cast<float>(m);
        const float noiseCoef = 0.02f + 0.98f * p.noiseColour * p.noiseColour;
        v.started = true;

        const dsp::MipmappedWave& saw = waves.shape(dsp::WaveformLibrary::Shape::Saw);
        const auto waveFor = [&](int o) -> const dsp::MipmappedWave& {
            switch (p.shape[o])
            {
            case Shape::Sine: return waves.shape(dsp::WaveformLibrary::Shape::Sine);
            case Shape::Triangle: return waves.shape(dsp::WaveformLibrary::Shape::Triangle);
            default: return saw;
            }
        };
        const dsp::MipmappedWave* wave[2] = {&waveFor(0), &waveFor(1)};
        const dsp::Wavetable* table[2] = {&waves.wavetable(p.table[0]), &waves.wavetable(p.table[1])};
        const auto oscSample = [&](int o, std::uint32_t phase, const dsp::MipmappedWave::Position& pos) {
            switch (p.shape[o])
            {
            case Shape::Pulse: return saw.sample(phase, pos) - saw.sample(phase + pwOff[o], pos);
            case Shape::Wavetable: return table[o]->sample(phase, pos, position[o]);
            default: return wave[o]->sample(phase, pos);
            }
        };
        const bool osc1On = level1 > 0.0f && oscHz[0] > 0.0;
        const bool osc2On = (level2 > 0.0f || p.sync) && oscHz[1] > 0.0;
        const bool sync = p.sync && osc2On;

        // The block is built in stages, each a tight loop: oscillators (the shape chosen outside the
        // loop), sub and noise, filter, then amplitude and pan.
        float* bufL = v.bufL.data();
        float* bufR = v.bufR.data();
        std::fill_n(bufL, m, 0.0f);
        std::fill_n(bufR, m, 0.0f);

        if (sync)
        {
            // Hard sync needs osc 1's wraps sample by sample: one combined loop.
            for (int i = 0; i < m; ++i)
            {
                float l = 0.0f;
                float r = 0.0f;
                for (int u = 0; u < unison; ++u)
                {
                    auto& c1 = v.osc1[static_cast<std::size_t>(u)];
                    auto& c2 = v.osc2[static_cast<std::size_t>(u)];
                    c1.inc = static_cast<std::uint32_t>(static_cast<std::int64_t>(c1.inc) + incStep[0][u]);
                    c2.inc = static_cast<std::uint32_t>(static_cast<std::int64_t>(c2.inc) + incStep[1][u]);

                    float s = 0.0f;
                    if (osc1On)
                        s += level1 * oscSample(0, c1.phase, at[0][u]);
                    const std::uint32_t before = c1.phase;
                    c1.phase += c1.inc;
                    const bool wrapped = c1.phase < before;

                    float y = oscSample(1, c2.phase, at[1][u]) + c2.blep;
                    c2.blep = 0.0f;
                    if (wrapped && c1.inc > 0)
                    {
                        // Osc 1 starts a new cycle tau of the way to the next sample, and osc 2
                        // restarts with it. A two-point polyBLEP spreads the jump over this sample and
                        // the next, so the restart does not alias.
                        const double tau = 1.0 - static_cast<double>(c1.phase) / static_cast<double>(c1.inc);
                        const auto partial = static_cast<std::uint32_t>(tau * c2.inc);
                        const float jump = oscSample(1, 0, at[1][u]) - oscSample(1, c2.phase + partial, at[1][u]);
                        const auto t = static_cast<float>(tau);
                        y += 0.5f * jump * (1.0f - t) * (1.0f - t);
                        c2.blep = -0.5f * jump * t * t;
                        c2.phase = static_cast<std::uint32_t>((1.0 - tau) * c2.inc);
                    }
                    else
                    {
                        c2.phase += c2.inc;
                    }
                    s += level2 * y;
                    l += s * (stereo ? p.unisonLeft[static_cast<std::size_t>(u)] : 1.0f);
                    r += s * (stereo ? p.unisonRight[static_cast<std::size_t>(u)] : 0.0f);
                }
                bufL[i] = l;
                bufR[i] = r;
            }
        }
        else
        {
            for (int o = 0; o < 2; ++o)
            {
                if (!(o == 0 ? osc1On : osc2On))
                    continue;
                const float level = o == 0 ? level1 : level2;
                auto& copies = o == 0 ? v.osc1 : v.osc2;
                int firstSingle = 0;
                // Unison: groups of four copies, one SSE lane each.
                if (p.shape[o] != Shape::Wavetable)
                {
                    const auto& w = p.shape[o] == Shape::Pulse ? saw : *wave[o];
                    for (int g = 0; g + 4 <= unison; g += 4)
                    {
                        // Copies detuned across a table boundary share the sparser table (alias-free
                        // for all of them; the richer one would only add top harmonics to some).
                        int groupLevel = 0;
                        bool usable = true;
                        for (int k = 0; k < 4; ++k)
                        {
                            usable = usable && !at[o][g + k].silent;
                            groupLevel = std::max(groupLevel, at[o][g + k].level);
                        }
                        for (int k = 0; k < 4; ++k)
                            usable = usable && at[o][g + k].level >= groupLevel - 1;
                        if (!usable)
                            break;
                        std::uint32_t phases[4];
                        std::uint32_t incs[4];
                        std::int32_t steps[4];
                        float blends[4];
                        float gl[4];
                        float gr[4];
                        for (int k = 0; k < 4; ++k)
                        {
                            const auto u = static_cast<std::size_t>(g + k);
                            phases[k] = copies[u].phase;
                            incs[k] = copies[u].inc;
                            steps[k] = static_cast<std::int32_t>(incStep[o][g + k]);
                            blends[k] = at[o][g + k].level == groupLevel ? at[o][g + k].blend : 0.0f;
                            gl[k] = level * (stereo ? p.unisonLeft[u] : 1.0f);
                            gr[k] = level * (stereo ? p.unisonRight[u] : 0.0f);
                        }
                        if (!dsp::renderFourCopies(w, groupLevel, blends, phases, incs, steps,
                                                   p.shape[o] == Shape::Pulse ? pwOff[o] : 0u, gl, gr, bufL, bufR, m))
                            break;
                        for (int k = 0; k < 4; ++k)
                        {
                            copies[static_cast<std::size_t>(g + k)].phase = phases[k];
                            copies[static_cast<std::size_t>(g + k)].inc = incs[k];
                        }
                        firstSingle = g + 4;
                    }
                }
                for (int u = firstSingle; u < unison; ++u)
                {
                    auto& c = (o == 0 ? v.osc1 : v.osc2)[static_cast<std::size_t>(u)];
                    const auto& pos = at[o][u];
                    const float gl = level * (stereo ? p.unisonLeft[static_cast<std::size_t>(u)] : 1.0f);
                    const float gr = level * (stereo ? p.unisonRight[static_cast<std::size_t>(u)] : 0.0f);
                    const std::int64_t step = incStep[o][u];
                    const auto run = [&](auto&& read) {
                        std::uint32_t phase = c.phase;
                        auto inc = static_cast<std::int64_t>(c.inc);
                        for (int i = 0; i < m; ++i)
                        {
                            inc += step;
                            const float x = read(phase);
                            phase += static_cast<std::uint32_t>(inc);
                            bufL[i] += x * gl;
                            bufR[i] += x * gr;
                        }
                        c.phase = phase;
                        c.inc = static_cast<std::uint32_t>(inc);
                    };
                    if (pos.silent)
                    {
                        run([](std::uint32_t) { return 0.0f; });
                        continue;
                    }
                    // The usual shapes: four samples at a time (SSE2).
                    if (p.shape[o] != Shape::Wavetable)
                    {
                        const bool pulse = p.shape[o] == Shape::Pulse;
                        if (dsp::renderCopy(pulse ? saw : *wave[o], pos, c.phase, c.inc, static_cast<std::int32_t>(step),
                                            pulse ? pwOff[o] : 0u, gl, gr, bufL, bufR, m))
                            continue;
                    }
                    switch (p.shape[o])
                    {
                    case Shape::Pulse:
                    {
                        const std::uint32_t off = pwOff[o];
                        if (pos.blend <= 0.0f)
                            run([&](std::uint32_t ph) {
                                return saw.readLevel(pos.level, ph) - saw.readLevel(pos.level, ph + off);
                            });
                        else
                            run([&](std::uint32_t ph) { return saw.sample(ph, pos) - saw.sample(ph + off, pos); });
                        break;
                    }
                    case Shape::Wavetable:
                    {
                        const auto* t = table[o];
                        const float morph = position[o];
                        run([&](std::uint32_t ph) { return t->sample(ph, pos, morph); });
                        break;
                    }
                    case Shape::Sine:
                    case Shape::Triangle:
                    case Shape::Saw:
                    {
                        const auto* w = wave[o];
                        if (pos.blend <= 0.0f)
                            run([&](std::uint32_t ph) { return w->readLevel(pos.level, ph); });
                        else
                            run([&](std::uint32_t ph) { return w->sample(ph, pos); });
                        break;
                    }
                    }
                }
            }
        }

        if (subLevel > 0.0f || noiseLevel > 0.0f)
        {
            const float side = stereo ? 0.70710678f : 1.0f;
            const auto& sine = waves.shape(dsp::WaveformLibrary::Shape::Sine);
            for (int i = 0; i < m; ++i)
            {
                float extra = 0.0f;
                if (subLevel > 0.0f)
                {
                    v.subInc = static_cast<std::uint32_t>(static_cast<std::int64_t>(v.subInc) + subStep);
                    extra += subLevel * sine.sample(v.subPhase, subAt);
                    v.subPhase += v.subInc;
                }
                if (noiseLevel > 0.0f)
                {
                    v.noiseSeed ^= v.noiseSeed << 13;
                    v.noiseSeed ^= v.noiseSeed >> 17;
                    v.noiseSeed ^= v.noiseSeed << 5;
                    const float white = static_cast<float>(v.noiseSeed) * 4.6566128730773926e-10f - 1.0f;
                    v.noiseState += noiseCoef * (white - v.noiseState);
                    extra += noiseLevel * v.noiseState;
                }
                bufL[i] += extra * side;
                bufR[i] += extra * side;
            }
        }

        // What the filter and amplitude stages need (they run across voices, in renderSegment).
        v.blockG = v.filterG;
        v.blockGStep = gStep;
        v.blockResonance = resonance;
        v.blockStereo = stereo;
        v.blockGain = velocityGain * ampMod * (stereo ? 1.41421356f : 1.0f);
        v.panFromL = panFromL;
        v.panFromR = panFromR;
        v.panStepL = panStepL;
        v.panStepR = panStepR;
        juce::ignoreUnused(start, ladder, steep, svfMode);

        // The increments ended exactly on target (integer division left a remainder).
        for (int u = 0; u < unison; ++u)
        {
            v.osc1[static_cast<std::size_t>(u)].inc = targetInc[0][u];
            v.osc2[static_cast<std::size_t>(u)].inc = targetInc[1][u];
        }
        v.subInc = subTarget;
        v.filterG = gTarget;
        v.lastPan = panTarget;
    }
}

void SynthDevice::finishVoiceBlock(Voice& v, float* outL, float* outR, int m) noexcept
{
    float* bufL = v.bufL.data();
    float* bufR = v.bufR.data();
    const bool stereo = v.blockStereo;
    if (patch.filter != FilterType::Ladder && patch.drive > 1.0f)
    {
        const float makeUp = 1.0f / std::sqrt(patch.drive);
        for (int i = 0; i < m; ++i)
        {
            bufL[i] = dsp::fastTanh(bufL[i]) * makeUp;
            bufR[i] = stereo ? dsp::fastTanh(bufR[i]) * makeUp : bufR[i];
        }
    }

    float ampBuf[controlBlock];
    v.amp.renderBlock(ampBuf, m);
    const float* rightSource = stereo ? bufR : bufL;
    float panL = v.panFromL;
    float panR = v.panFromR;
    for (int i = 0; i < m; ++i)
    {
        const float gain = ampBuf[i] * v.blockGain;
        panL += v.panStepL;
        panR += v.panStepR;
        outL[i] += bufL[i] * gain * panL;
        outR[i] += rightSource[i] * gain * panR;
    }
}

} // namespace lattice
