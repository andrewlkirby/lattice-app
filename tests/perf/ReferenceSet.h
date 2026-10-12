#pragma once

// The performance reference set (docs/09-performance-charter.md §1, "The reference set"), as it
// stands at M3: 32 tracks with clips in 8 scenes, built from the M3 devices.
// - 16 "Synth" tracks: the Synth (chords plus a 16th-note line, about 8 voices each) and an EQ
//   Eight; the first four duck under a sidechain compressor keyed from the first drum track.
// - 8 "Drums" tracks: short, noisy Synth hits on 16ths.
// - 8 "Sampler" tracks: an 8th-note bass line (a Synth stands in until the set can carry sample
//   files; tests/perf/reference-set/README.md).
// - Every track sends to return A (Reverb) and the odd ones to return B (Delay); a Limiter on the
//   master.
// It grows with each milestone; tests/perf/reference-set/ holds the generated .lset for the app.

#include "core/model/LiveSet.h"
#include "devices/effects/Compressor.h"
#include "devices/effects/Delay.h"
#include "devices/effects/EqEight.h"
#include "devices/effects/Limiter.h"
#include "devices/effects/Reverb.h"

namespace lattice::perf
{

inline constexpr int referenceTracks = 32;
inline constexpr int referenceScenes = 8;

inline void buildReferenceSet(LiveSet& set)
{
    set.resetToDefault(referenceTracks, referenceScenes);
    set.setTempo(124.0);
    set.setGlobalQuantization(Quantization::Bar1);

    const auto add = [&set](const juce::ValueTree& clip, int pitch, double start, double length,
                            int velocity) {
        NoteData n;
        n.pitch = pitch;
        n.start = start;
        n.length = length;
        n.velocity = velocity;
        set.addNote(clip, n);
    };

    // Returns and master.
    while (set.numReturns() < 2)
        set.addReturn();
    set.insertDevice(LiveSet::chainOf(set.returnTrack(0)), -1, ReverbDevice::typeId);
    set.insertDevice(LiveSet::chainOf(set.returnTrack(1)), -1, DelayDevice::typeId);
    set.insertDevice(LiveSet::chainOf(set.master()), -1, LimiterDevice::typeId);
    const std::int64_t kickTrack = set.trackId(16);

    for (int t = 0; t < referenceTracks; ++t)
    {
        set.setTrackArm(t, false, true);
        const auto chain = LiveSet::chainOf(set.track(t));
        const auto synthDevice = chain.getChild(0); // the default instrument
        set.setSendLevel(t, 0, -15.0);
        if (t % 2 == 1)
            set.setSendLevel(t, 1, -21.0);
        if (t < 16)
        {
            set.insertDevice(chain, -1, EqEightDevice::typeId);
            if (t < 4)
            {
                auto comp = set.insertDevice(chain, -1, CompressorDevice::typeId);
                set.setDeviceParameter(comp, "comp.sidechain", 1.0);
                comp.getChildWithName(ids::deviceState).setProperty(ids::sidechainTrack, static_cast<juce::int64>(kickTrack), nullptr);
            }
        }
        else if (t < 24)
        {
            // A short, noisy hit.
            set.setDeviceParameter(synthDevice, "noise.level", -6.0);
            set.setDeviceParameter(synthDevice, "amp.decay", 90.0);
            set.setDeviceParameter(synthDevice, "amp.sustain", 0.0);
            set.setDeviceParameter(synthDevice, "amp.release", 60.0);
            set.setDeviceParameter(synthDevice, "filter.type", 6.0); // the ladder
        }
        else
        {
            set.setDeviceParameter(synthDevice, "osc1.shape", 3.0); // pulse
            set.setDeviceParameter(synthDevice, "sub.level", -6.0);
            set.setDeviceParameter(synthDevice, "filter.cutoff", 800.0);
        }
        const bool synth = t < 16;
        const bool drums = t >= 16 && t < 24;
        set.setTrackName(t, synth   ? "Synth " + juce::String(t + 1)
                            : drums ? "Drums " + juce::String(t - 15)
                                    : "Sampler " + juce::String(t - 23));
        set.setTrackVolumeDb(t, -18.0);
        set.setTrackPan(t, ((t % 5) - 2) * 0.3);

        for (int s = 0; s < referenceScenes; ++s)
        {
            auto clip = set.createClip(t, s, 16.0, "Scene " + juce::String(s + 1));
            const int root = 36 + (t * 7 + s * 5) % 24;
            for (int bar = 0; bar < 4; ++bar)
            {
                const double b = bar * 4.0;
                if (synth)
                {
                    // A four-note chord per bar plus a 16th-note line: about 8 voices with tails.
                    for (const int interval : {0, 4, 7, 11})
                        add(clip, root + 12 + interval + (bar % 2) * 2, b, 3.75, 70);
                    for (int step = 0; step < 16; ++step)
                        add(clip, root + 24 + (step * 5) % 12, b + step * 0.25, 0.2, 60 + step % 4 * 10);
                }
                else if (drums)
                {
                    for (int step = 0; step < 16; ++step)
                        add(clip,
                            60 + (step % 4 == 0   ? 0
                                  : step % 2 == 0 ? 2
                                                  : 6),
                            b + step * 0.25, 0.05, step % 4 == 0 ? 120 : 80);
                }
                else
                {
                    for (int step = 0; step < 8; ++step)
                        add(clip, root + (step % 3 == 0 ? 0 : 7), b + step * 0.5, 0.4, 100);
                }
            }
        }
    }
}

} // namespace lattice::perf
