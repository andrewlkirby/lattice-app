#pragma once

// The performance reference set (docs/09-performance-charter.md §1, "The reference set"), as it
// stands at M2: 32 tracks, all playing the test synth as a placeholder for the M3 devices, with
// clips in 8 scenes. 16 "Synth" tracks hold chords plus a 16th-note line (about 8 voices each),
// 8 "Drums" tracks short 16th-note hits, and 8 "Sampler" tracks an 8th-note bass line. It grows
// with each milestone; tests/perf/reference-set/ holds the generated .lset for opening in the app.

#include "core/model/LiveSet.h"

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

    for (int t = 0; t < referenceTracks; ++t)
    {
        set.setTrackArm(t, false, true);
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
