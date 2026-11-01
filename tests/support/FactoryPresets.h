#pragma once

// The factory presets (docs/04-m3-devices.md, "3f"), built through the same editing calls as the
// app. resources/presets/*.lpreset is generated from these: after changing one, run
//
//   lattice_tests "[.write-factory-presets]"
//
// and commit the files; a test fails while the embedded presets and these builders disagree.

#include "core/model/LiveSet.h"
#include "devices/effects/Delay.h"
#include "devices/instruments/Synth.h"
#include "devices/midi/Arpeggiator.h"
#include "devices/midi/Chord.h"
#include "devices/presets/Presets.h"
#include "devices/racks/Rack.h"
#include "devices/racks/RackEditing.h"

#include <functional>
#include <vector>

namespace lattice::test
{

struct FactoryPreset
{
    juce::String name;
    juce::String file;                                           // in resources/presets/
    std::function<juce::ValueTree(LiveSet&, juce::ValueTree)> build; // (set, chain) -> device
};

inline void setParams(LiveSet& set, const juce::ValueTree& device,
                      std::initializer_list<std::pair<const char*, double>> values)
{
    for (const auto& [id, v] : values)
        set.setDeviceParameter(device, id, v);
}

/** A slow, wide pad. Colundi's steps are not semitones: the second oscillator sits 12 table steps
    up (near an octave low in the table, a little under one higher up), so the pad's colour shifts
    with the register, as the scale does. */
inline juce::ValueTree buildColundiPad(LiveSet& set, juce::ValueTree chain)
{
    auto synth = set.insertDevice(std::move(chain), -1, SynthDevice::typeId);
    setParams(set, synth,
              {{"osc1.shape", 2.0}, // saw
               {"osc1.level", -6.0},
               {"osc2.shape", 4.0}, // wavetable
               {"osc2.level", -9.0},
               {"osc2.transpose", 12.0},
               {"osc2.detune", 6.0},
               {"osc2.position", 0.35},
               {"unison.voices", 3.0},
               {"unison.detune", 14.0},
               {"unison.spread", 0.9},
               {"filter.type", 1.0}, // low-pass 24
               {"filter.cutoff", 1600.0},
               {"filter.resonance", 0.22},
               {"filter.keytrack", 0.4},
               {"filter.envAmount", 1.2},
               {"env2.attack", 900.0},
               {"env2.decay", 2500.0},
               {"env2.sustain", 0.3},
               {"env2.release", 2000.0},
               {"amp.attack", 700.0},
               {"amp.decay", 1500.0},
               {"amp.sustain", 0.85},
               {"amp.release", 2200.0},
               {"lfo1.shape", 0.0}, // sine
               {"lfo1.rate", 0.18},
               {"mod1.source", 2.0}, // LFO 1
               {"mod1.dest", 10.0},  // cutoff
               {"mod1.amount", 0.18},
               {"lfo2.shape", 1.0}, // triangle
               {"lfo2.rate", 0.07},
               {"mod2.source", 3.0}, // LFO 2
               {"mod2.dest", 7.0},   // osc 2 position
               {"mod2.amount", 0.4},
               {"out.volume", -10.0}});
    return synth;
}

/** A mono bass with glide through the table: the ladder filter, a sub oscillator, a short filter
    envelope. */
inline juce::ValueTree buildColundiBass(LiveSet& set, juce::ValueTree chain)
{
    auto synth = set.insertDevice(std::move(chain), -1, SynthDevice::typeId);
    setParams(set, synth,
              {{"osc1.shape", 2.0}, // saw
               {"osc1.level", -4.0},
               {"osc2.shape", 3.0}, // pulse
               {"osc2.level", -10.0},
               {"osc2.transpose", -12.0},
               {"osc2.pw", 0.3},
               {"sub.level", -8.0},
               {"filter.type", 6.0}, // ladder
               {"filter.cutoff", 320.0},
               {"filter.resonance", 0.35},
               {"filter.drive", 6.0},
               {"filter.keytrack", 0.6},
               {"filter.envAmount", 2.8},
               {"filter.velocity", 1.0},
               {"env2.attack", 1.0},
               {"env2.decay", 220.0},
               {"env2.sustain", 0.1},
               {"env2.release", 150.0},
               {"amp.attack", 1.0},
               {"amp.decay", 500.0},
               {"amp.sustain", 0.7},
               {"amp.release", 90.0},
               {"voice.mode", 2.0}, // legato
               {"voice.glide", 70.0},
               {"out.volume", -8.0}});
    return synth;
}

/** An Instrument Rack: Chord (+3 and +7 table steps, a Colundi "triad") into an Arpeggiator into
    a plucked Synth and a ping-pong Delay, with 8 macros. */
inline juce::ValueTree buildColundiArpChordRack(LiveSet& set, juce::ValueTree chain)
{
    auto rack = racks::insertRack(set, std::move(chain), -1, RackDevice::instrumentRackId);
    auto inner = racks::devicesOf(racks::addChain(set, rack, "Arp"));
    auto chord = set.insertDevice(inner, -1, ChordDevice::typeId);
    setParams(set, chord, {{"chord.shift1", 3.0}, {"chord.shift2", 7.0}, {"chord.velocity2", 0.8}});
    auto arp = set.insertDevice(inner, -1, ArpeggiatorDevice::typeId);
    setParams(set, arp, {{"arp.style", 2.0}, // up/down
                         {"arp.rate", 13.0}, // 1/16
                         {"arp.gate", 0.6},
                         {"arp.range", 2.0},
                         {"arp.distance", 12.0}});
    auto synth = set.insertDevice(inner, -1, SynthDevice::typeId);
    setParams(set, synth,
              {{"osc1.shape", 1.0}, // triangle
               {"osc1.level", -4.0},
               {"osc2.shape", 2.0}, // saw
               {"osc2.level", -12.0},
               {"osc2.detune", 4.0},
               {"filter.type", 1.0}, // low-pass 24
               {"filter.cutoff", 2400.0},
               {"filter.resonance", 0.25},
               {"filter.envAmount", 2.0},
               {"env2.decay", 180.0},
               {"env2.sustain", 0.0},
               {"amp.attack", 1.0},
               {"amp.decay", 320.0},
               {"amp.sustain", 0.0},
               {"amp.release", 260.0},
               {"out.volume", -9.0}});
    auto delay = set.insertDevice(inner, -1, DelayDevice::typeId);
    setParams(set, delay, {{"delay.mode", 1.0}, // ping-pong
                           {"delay.syncLeft", 9.0},  // 1/8D
                           {"delay.syncRight", 9.0},
                           {"delay.feedback", 0.35},
                           {"delay.highCut", 6000.0},
                           {"delay.mix", 0.22}});

    racks::setMacroCount(set, rack, 8);
    const auto macro = [&](int m, const char* name, const juce::ValueTree& device, const char* parameter,
                           double low, double high) {
        racks::setMacroName(set, rack, m, name);
        racks::addMapping(set, rack, m, device, parameter, low, high);
    };
    macro(0, "Cutoff", synth, "filter.cutoff", 0.25, 0.95);
    macro(1, "Resonance", synth, "filter.resonance", 0.0, 0.85);
    macro(2, "Decay", synth, "amp.decay", 0.05, 0.6);
    macro(3, "Arp Rate", arp, "arp.rate", 0.45, 0.85);
    macro(4, "Arp Gate", arp, "arp.gate", 0.05, 0.8);
    macro(5, "Arp Style", arp, "arp.style", 0.0, 1.0);
    macro(6, "Delay Mix", delay, "delay.mix", 0.0, 0.6);
    macro(7, "Feedback", delay, "delay.feedback", 0.0, 0.85);
    // The macros' starting positions (0..127): mapped parameters follow them, so these, not the
    // values set on the devices above, are what the preset plays with.
    set.setDeviceParameter(rack, "macro1", 64.0);
    set.setDeviceParameter(rack, "macro2", 38.0);
    set.setDeviceParameter(rack, "macro3", 64.0);
    set.setDeviceParameter(rack, "macro4", 64.0);
    set.setDeviceParameter(rack, "macro5", 64.0);
    set.setDeviceParameter(rack, "macro6", 51.0);
    set.setDeviceParameter(rack, "macro7", 47.0);
    set.setDeviceParameter(rack, "macro8", 52.0);
    return rack;
}

// --- Drum Rack kits: every pad a Synth, so they need no samples ----------------------------------

using Params = std::initializer_list<std::pair<const char*, double>>;

/** A Drum Rack pad on `note` holding a Synth set up for one-shot drums (no sustain, no second
    oscillator, no key tracking), playing `playNote`. Fixed: 12-TET from middle C, whatever the
    track's tuning (kicks, snares and hats keep their pitch); otherwise it follows the track's
    tuning, as Colundi toms and bells do. */
inline juce::ValueTree addSynthPad(LiveSet& set, const juce::ValueTree& kit, int note, const char* name, int playNote,
                                   bool fixed, int chokeGroup, Params params)
{
    auto pad = racks::addChain(set, kit, name);
    pad.setProperty(ids::receiveNote, note, set.undoManager());
    racks::setPadPlayNote(set, pad, playNote);
    racks::setChokeGroup(set, pad, chokeGroup);
    auto synth = set.insertDevice(racks::devicesOf(pad), -1, SynthDevice::typeId);
    setParams(set, synth,
              {{"osc1.shape", 0.0}, // sine
               {"osc1.level", 0.0},
               {"osc2.level", -70.0},
               {"filter.type", 0.0}, // low-pass 12
               {"filter.cutoff", 16000.0},
               {"filter.keytrack", 0.0},
               {"amp.attack", 0.5},
               {"amp.sustain", 0.0},
               {"amp.velocity", 0.6},
               {"env2.attack", 0.5},
               {"voice.polyphony", 4.0},
               {"voice.tuning", fixed ? 1.0 : 0.0}});
    setParams(set, synth, params);
    // Drums ring out however short the note: the release is the decay.
    set.setDeviceParameter(synth, "amp.release", static_cast<double>(synth.getChildWithName(ids::params)["amp.decay"]));
    return pad;
}

/** Analog-style drums on the General MIDI pads (kick 36, snare 38, hats 42 and 46, toms 41, 45
    and 48, ...), all at fixed pitch: the same in every tuning. The hats choke each other. */
inline juce::ValueTree buildSynthKit(LiveSet& set, juce::ValueTree chain)
{
    auto kit = racks::insertRack(set, std::move(chain), -1, RackDevice::drumRackId);
    constexpr bool fixed = true;
    addSynthPad(set, kit, 36, "Kick", 31, fixed, 0, // 49 Hz, from about 150
                {{"env2.decay", 45.0}, {"env2.sustain", 0.0},
                 {"mod1.source", 1.0}, {"mod1.dest", 1.0}, {"mod1.amount", 0.8}, // Env 2 -> pitch
                 {"amp.decay", 700.0}, {"amp.velocity", 0.4},
                 {"out.volume", -2.0}});
    addSynthPad(set, kit, 37, "Rim", 90, fixed, 0,
                {{"osc1.shape", 1.0}, {"osc1.level", 0.0}, // triangle
                 {"osc2.shape", 0.0}, {"osc2.level", -6.0}, {"osc2.transpose", -5.0},
                 {"noise.level", -18.0},
                 {"filter.type", 2.0}, {"filter.cutoff", 400.0}, // high-pass 12
                 {"amp.decay", 28.0},
                 {"out.volume", 2.0}});
    addSynthPad(set, kit, 38, "Snare", 54, fixed, 0, // a 185 Hz body under noise
                {{"osc1.shape", 1.0}, {"osc1.level", -6.0},
                 {"noise.level", -3.0}, {"noise.colour", 1.0},
                 {"filter.type", 2.0}, {"filter.cutoff", 180.0}, {"filter.resonance", 0.1},
                 {"env2.decay", 25.0}, {"env2.sustain", 0.0},
                 {"mod1.source", 1.0}, {"mod1.dest", 1.0}, {"mod1.amount", 0.25},
                 {"amp.decay", 200.0},
                 {"out.volume", -5.0}});
    addSynthPad(set, kit, 39, "Clap", 60, fixed, 0, // band-passed noise, chopped by a falling saw
                {{"osc1.level", -70.0},
                 {"noise.level", 0.0}, {"noise.colour", 0.9},
                 {"filter.type", 4.0}, {"filter.cutoff", 1100.0}, {"filter.resonance", 0.35}, // band-pass
                 {"lfo1.shape", 3.0}, {"lfo1.rate", 38.0}, {"lfo1.retrigger", 1.0}, // saw down
                 {"mod1.source", 2.0}, {"mod1.dest", 13.0}, {"mod1.amount", 0.7},     // LFO 1 -> amp
                 {"amp.decay", 260.0},
                 {"out.volume", -4.0}});
    const auto tom = [&](int note, const char* name, int playNote) {
        addSynthPad(set, kit, note, name, playNote, fixed, 0,
                    {{"noise.level", -26.0}, {"noise.colour", 0.5},
                     {"filter.cutoff", 4000.0},
                     {"env2.decay", 90.0}, {"env2.sustain", 0.0},
                     {"mod1.source", 1.0}, {"mod1.dest", 1.0}, {"mod1.amount", 0.22},
                     {"amp.decay", 700.0},
                     {"out.volume", -4.0}});
    };
    tom(41, "Low Tom", 43);
    const auto hat = [&](int note, const char* name, double decayMs, double volume) {
        addSynthPad(set, kit, note, name, 103, fixed, 1, // two pulses a fifth apart for the metal
                    {{"osc1.shape", 3.0}, {"osc1.level", -18.0},
                     {"osc2.shape", 3.0}, {"osc2.level", -20.0}, {"osc2.transpose", 7.0},
                     {"noise.level", 0.0}, {"noise.colour", 1.0},
                     {"filter.type", 3.0}, {"filter.cutoff", 8000.0}, {"filter.resonance", 0.15}, // high-pass 24
                     {"amp.decay", decayMs},
                     {"out.volume", volume}});
    };
    hat(42, "Closed Hat", 55.0, -6.0);
    tom(45, "Mid Tom", 48);
    hat(46, "Open Hat", 420.0, -10.0);
    tom(48, "High Tom", 53);
    addSynthPad(set, kit, 49, "Crash", 60, fixed, 0,
                {{"osc1.level", -70.0},
                 {"noise.level", 0.0}, {"noise.colour", 1.0},
                 {"filter.type", 2.0}, {"filter.cutoff", 3500.0}, {"filter.resonance", 0.1},
                 {"amp.decay", 1800.0},
                 {"out.volume", -12.0}});
    addSynthPad(set, kit, 51, "Ride", 100, fixed, 0,
                {{"osc1.shape", 3.0}, {"osc1.level", -14.0},
                 {"osc2.shape", 3.0}, {"osc2.level", -16.0}, {"osc2.transpose", 6.0},
                 {"noise.level", -4.0}, {"noise.colour", 1.0},
                 {"filter.type", 3.0}, {"filter.cutoff", 5000.0}, {"filter.resonance", 0.25},
                 {"amp.decay", 1100.0},
                 {"out.volume", -13.0}});
    addSynthPad(set, kit, 56, "Cowbell", 73, fixed, 0, // 554 and 831 Hz
                {{"osc1.shape", 3.0}, {"osc1.level", -4.0},
                 {"osc2.shape", 3.0}, {"osc2.level", -4.0}, {"osc2.transpose", 7.0},
                 {"filter.type", 4.0}, {"filter.cutoff", 850.0}, {"filter.resonance", 0.45},
                 {"amp.decay", 240.0},
                 {"out.volume", -9.0}});
    return kit;
}

/** Kick, snare and hats at fixed pitch, and toms and bells that follow the track's tuning: on a
    Colundi track they sit on the table's steps (toms at 80, 96, 110 and 132 Hz, bells at 384, 440,
    528 and 639 Hz), with a second oscillator 12 steps up. */
inline juce::ValueTree buildColundiKit(LiveSet& set, juce::ValueTree chain)
{
    auto kit = racks::insertRack(set, std::move(chain), -1, RackDevice::drumRackId);
    constexpr bool fixed = true, tuned = false;
    addSynthPad(set, kit, 36, "Kick", 29, fixed, 0, // 44 Hz, deep and long
                {{"env2.decay", 60.0}, {"env2.sustain", 0.0},
                 {"mod1.source", 1.0}, {"mod1.dest", 1.0}, {"mod1.amount", 0.75},
                 {"filter.drive", 4.0},
                 {"amp.decay", 1200.0}, {"amp.velocity", 0.4},
                 {"out.volume", 0.0}});
    addSynthPad(set, kit, 38, "Snare", 50, fixed, 0, // lower, noisier
                {{"osc1.shape", 1.0}, {"osc1.level", -8.0},
                 {"noise.level", -2.0}, {"noise.colour", 0.8},
                 {"filter.type", 4.0}, {"filter.cutoff", 2400.0}, {"filter.resonance", 0.2},
                 {"env2.decay", 35.0}, {"env2.sustain", 0.0},
                 {"mod1.source", 1.0}, {"mod1.dest", 1.0}, {"mod1.amount", 0.3},
                 {"amp.decay", 260.0},
                 {"out.volume", 2.0}});
    const auto hat = [&](int note, const char* name, double decayMs, double volume) {
        addSynthPad(set, kit, note, name, 60, fixed, 1,
                    {{"osc1.level", -70.0},
                     {"noise.level", 0.0}, {"noise.colour", 1.0},
                     {"filter.type", 3.0}, {"filter.cutoff", 9500.0}, {"filter.resonance", 0.3},
                     {"amp.decay", decayMs},
                     {"out.volume", volume}});
    };
    const auto tom = [&](int note, const char* name, int step) {
        addSynthPad(set, kit, note, name, step, tuned, 0,
                    {{"osc2.shape", 1.0}, {"osc2.level", -16.0}, {"osc2.transpose", 12.0},
                     {"filter.cutoff", 3000.0},
                     {"env2.decay", 120.0}, {"env2.sustain", 0.0},
                     {"mod1.source", 1.0}, {"mod1.dest", 1.0}, {"mod1.amount", 0.12},
                     {"amp.decay", 900.0},
                     {"out.volume", -5.0}});
    };
    const auto bell = [&](int note, const char* name, int step) {
        addSynthPad(set, kit, note, name, step, tuned, 0,
                    {{"osc2.shape", 0.0}, {"osc2.level", -10.0}, {"osc2.transpose", 12.0},
                     {"env2.decay", 300.0}, {"env2.sustain", 0.0},
                     {"mod1.source", 1.0}, {"mod1.dest", 9.0}, {"mod1.amount", 0.6}, // Env 2 -> osc 2 level
                     {"amp.decay", 1400.0},
                     {"out.volume", -12.0}});
    };
    tom(41, "Tom 1", 17);
    hat(42, "Closed Hat", 40.0, -6.0);
    tom(43, "Tom 2", 20);
    tom(45, "Tom 3", 23);
    hat(46, "Open Hat", 360.0, -11.0);
    tom(47, "Tom 4", 26);
    bell(48, "Bell 1", 44);
    bell(50, "Bell 2", 47);
    bell(52, "Bell 3", 50);
    bell(53, "Bell 4", 53);
    return kit;
}

inline const std::vector<FactoryPreset>& factoryPresets()
{
    static const std::vector<FactoryPreset> all{
        {"Colundi Pad", "Colundi Pad.lpreset", buildColundiPad},
        {"Colundi Bass", "Colundi Bass.lpreset", buildColundiBass},
        {"Colundi Arp Chords", "Colundi Arp Chords.lpreset", buildColundiArpChordRack},
        {"Synth Kit", "Synth Kit.lpreset", buildSynthKit},
        {"Colundi Kit", "Colundi Kit.lpreset", buildColundiKit},
    };
    return all;
}

/** The preset file's text, built in a fresh set (so its IDs are the same every time). */
inline juce::String buildFactoryPresetXml(const FactoryPreset& preset)
{
    LiveSet set;
    set.resetToDefault(1, 1);
    auto chain = LiveSet::chainOf(set.track(0));
    while (chain.getNumChildren() > 0)
        set.removeDevice(chain.getChild(0));
    const auto device = preset.build(set, chain);
    return presets::toXmlString(device, preset.name);
}

} // namespace lattice::test
