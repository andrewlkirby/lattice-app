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

inline void setParams(LiveSet& set, juce::ValueTree device,
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
    auto synth = set.insertDevice(chain, -1, SynthDevice::typeId);
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
    auto synth = set.insertDevice(chain, -1, SynthDevice::typeId);
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
    auto rack = racks::insertRack(set, chain, -1, RackDevice::instrumentRackId);
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

inline const std::vector<FactoryPreset>& factoryPresets()
{
    static const std::vector<FactoryPreset> all{
        {"Colundi Pad", "Colundi Pad.lpreset", buildColundiPad},
        {"Colundi Bass", "Colundi Bass.lpreset", buildColundiBass},
        {"Colundi Arp Chords", "Colundi Arp Chords.lpreset", buildColundiArpChordRack},
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
