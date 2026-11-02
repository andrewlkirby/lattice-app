#pragma once

#include "core/RealtimeCheck.h"
#include "devices/samples/SamplePool.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

namespace lattice
{

/** The browser's preview bus (docs/05-m4-ui.md, 4e): plays one sound at a time, mixed after the
    master (it never goes through a track, the master's devices or its meter), at its own volume.

    - play() hands a decoded sound to the audio thread without locks: the message thread keeps every
      sound alive until the audio thread says it's done with it (it fades the old one out over a few
      milliseconds while the new one starts, so switching never clicks).
    - A sound is resampled to the device's rate as it plays.
    - Synced, a sound waits for the next bar while the set plays (without the set playing it starts
      at once). Warping it to the tempo waits for time-stretching (post-MVP). */
class PreviewPlayer
{
public:
    static constexpr int fadeFrames = 256;

    /** Message thread. A null sample stops what's playing (with a short fade). Returns the sound's
        generation (what playingGeneration() says once it sounds). */
    std::uint64_t play(SamplePtr sound, bool syncToBar = false);
    void stop() { play(nullptr); }

    /** Linear, 0..1. Any thread. */
    void setGain(float linear) noexcept { gain.store(linear, std::memory_order_relaxed); }
    float getGain() const noexcept { return gain.load(std::memory_order_relaxed); }

    /** Any thread: whether the last sound given is playing (or waiting for its bar), and where. */
    bool isPlaying() const noexcept { return playingNow.load(std::memory_order_relaxed); }
    std::int64_t position() const noexcept { return frame.load(std::memory_order_relaxed); }
    /** Any thread: which play() call is sounding (1 for the first; 0 before any). */
    std::uint64_t playingGeneration() const noexcept { return acked.load(std::memory_order_acquire); }

    /** Message thread: frees sounds the audio thread has finished with. */
    void collectGarbage();
    std::size_t soundsKept() const noexcept { return alive.size(); } // tests

    /** Not on the audio thread. */
    void prepare(double sampleRate) noexcept;

    /** Audio thread. Adds the preview to left/right. samplesToNextBar: how far the next bar is while
        the set plays (negative when it isn't playing). */
    void render(float* left, float* right, int numSamples, double samplesToNextBar) noexcept LATTICE_NONBLOCKING;

private:
    struct Sound
    {
        std::uint64_t generation = 0;
        SamplePtr sample;
        bool sync = false;
    };
    struct Voice
    {
        const Sound* sound = nullptr;
        double position = 0.0;  // frames into the sample
        std::int64_t wait = 0;  // frames before it starts (a synced start)
        int fade = 0;           // frames of fading out left (0: not fading)
    };

    void renderVoice(Voice& v, float* left, float* right, int numSamples, float g0, float g1, bool fadingOut) noexcept;

    // Message thread.
    std::vector<std::shared_ptr<const Sound>> alive;
    std::uint64_t nextGeneration = 1;

    std::atomic<const Sound*> latest{nullptr};
    std::atomic<std::uint64_t> acked{0}; // the oldest generation the audio thread still uses
    std::atomic<float> gain{0.7f};
    std::atomic<bool> playingNow{false};
    std::atomic<std::int64_t> frame{-1};

    // Audio thread.
    double sampleRate = 48000.0;
    Voice current, fading;
    float lastGain = 0.7f;
};

} // namespace lattice
