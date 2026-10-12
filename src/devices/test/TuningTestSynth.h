#pragma once

#include "core/RealtimeCheck.h"
#include "engine/MidiInputQueue.h"
#include "tuning/TrackTuning.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cstdint>

namespace lattice
{

/** Temporary instrument for hearing tunings until the M3 devices exist (docs/02-m1-tuning-core.md).
    16 voices, each a sine plus a softened band-limited saw with an ADSR. Every voice takes its pitch
    from TrackTuning::hzFor, the way every M3 instrument will.

    Written instead of using juce::Synthesiser, which takes a lock on the audio thread. Rendering
    is lock-free and allocation-free; MIDI events are applied at their sample offsets. */
class TuningTestSynth
{
public:
    static constexpr int numVoices = 16;

    explicit TuningTestSynth(const TrackTuning& tuning);

    /** Not on the audio thread. */
    void prepare(double sampleRate);

    /** 0 = pure sine, 1 = full saw. Any thread. */
    void setSawMix(float mix) noexcept { sawMix.store(juce::jlimit(0.0f, 1.0f, mix)); }

    /** Audio thread. Overwrites out[0..numSamples) and applies the events (sorted by sampleOffset,
        each within the block) at their positions. */
    void render(float* out, int numSamples, const TimedMidi* events, int numEvents) noexcept LATTICE_NONBLOCKING;

    /** Audio thread. Releases every voice (with its release stage). */
    void allNotesOff() noexcept LATTICE_NONBLOCKING;

    /** Last note-on that actually started a voice, or -1. Any thread. */
    int lastNoteOn() const noexcept { return lastNote.load(std::memory_order_relaxed); }

    /** Number of sounding voices (for tests and meters). Audio thread or tests. */
    int activeVoiceCount() const noexcept;

private:
    struct Voice
    {
        int note = -1;
        bool held = false;
        double phase = 0.0;   // cycles, in [0, 1)
        double hz = 0.0;
        float gain = 0.0f;
        float sawState = 0.0f; // one-pole low-pass state for the "soft" saw
        std::uint64_t startedAt = 0;
        juce::ADSR envelope;
    };

    void handleEvent(const TimedMidi& e) noexcept LATTICE_NONBLOCKING;
    void noteOn(int note, int velocity) noexcept LATTICE_NONBLOCKING;
    void noteOff(int note) noexcept LATTICE_NONBLOCKING;
    void renderVoices(float* out, int numSamples) noexcept LATTICE_NONBLOCKING;

    const TrackTuning& tuning;
    std::array<Voice, numVoices> voices;
    double sampleRate = 48000.0;
    float bend = 0.0f;           // -1..1 from the pitch wheel
    std::uint64_t noteCounter = 0;
    std::atomic<float> sawMix{0.35f};
    std::atomic<int> lastNote{-1};
};

} // namespace lattice
