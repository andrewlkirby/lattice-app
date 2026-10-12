#pragma once

#include "core/RealtimeCheck.h"
#include "core/Seqlock.h"
#include "core/SettledShared.h"
#include "devices/dsp/DelayLine.h"
#include "devices/framework/DeviceChain.h"
#include "engine/session/ClipPlayer.h"
#include "tuning/TrackTuning.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace lattice
{

/** Receives every MIDI event a track's instrument gets, with its absolute sample position. For
    tests (the OfflineRenderer); called on the audio thread, so it must not allocate. */
struct TrackEventTap
{
    virtual ~TrackEventTap() = default;
    virtual void onTrackEvent(std::int64_t trackId, std::int64_t absoluteSample,
                              const TimedMidi& event) noexcept = 0;
};

/** What the UI shows for a track's clip launcher, published by the audio thread. */
struct TrackPlayState
{
    enum class Queued : std::uint8_t
    {
        None,
        Launch,
        Stop,
        Record
    };

    std::int64_t playingClipId = 0;
    std::int64_t queuedClipId = 0; // the clip of a queued launch or recording
    std::int32_t queuedScene = -1;
    Queued queued = Queued::None;
    bool recording = false;
    double position = 0.0; // clip position of the playing clip, in beats
    double loopStart = 0.0;
    double loopEnd = 0.0;
};

/** Audio-thread session state of one track: what the scheduler and the recorder work on.
    Owned by TrackProcessor, touched only by the audio thread (SessionProcessor). */
struct TrackSessionState
{
    static constexpr double never = std::numeric_limits<double>::infinity();

    struct Pending
    {
        TrackPlayState::Queued kind = TrackPlayState::Queued::None;
        std::int64_t clipId = 0;
        std::int32_t scene = -1;
        double beat = never;
        bool fromFollowAction = false;
        bool openEnded = false; // Record: open-length recording
    };

    ClipPlayer player;
    Pending pending;
    bool stopAfterLaunch = false; // Gate: released before the queued launch happened
    double followBeat = never;    // when the playing clip's follow action fires
    double endRecordingBeat = never;

    // Recording a new clip (as opposed to overdubbing a playing one)
    bool recordingNewClip = false;
    bool recordingOpenEnded = false;
    double recordingStartBeat = 0.0;
    double recordingEndBeat = never; // fixed length: when it turns into playback

    // Capturing input into a clip (a new recording or an overdub)
    std::int64_t captureClipId = 0;
    std::array<bool, 128> captureHeld{};
};

/** One track in the engine: device chain, tuning, playback state, mixer gain and meters
    (docs/03-m2-engine-session.md, part 2b; docs/04-m3-devices.md, "Chain changes"). Created by the
    SnapshotBuilder on the message thread and kept per track ID across snapshots, so rebuilding a
    snapshot never recreates the devices or cuts off their sound. Shared by the snapshots that list
    it; freed on the message thread.

    The device chain is published separately from the engine snapshot (setChain()). When it
    changes, the audio thread fades the old chain out over chainFadeSeconds, switches, and fades the
    new one in, so inserting, removing or reordering devices never clicks. The old chain stays alive
    until the audio thread has switched (SettledShared). */
class TrackProcessor
{
public:
    static constexpr int maxEventsPerChunk = 4096;
    static constexpr double gainRampSeconds = 0.02;
    static constexpr double chainFadeSeconds = 0.003;

    explicit TrackProcessor(std::int64_t trackId);
    ~TrackProcessor();

    TrackProcessor(const TrackProcessor&) = delete;
    TrackProcessor& operator=(const TrackProcessor&) = delete;

    std::int64_t trackId() const noexcept { return id; }
    TrackTuning& tuning() noexcept { return trackTuning; }

    /** Not on the audio thread (and not while the audio thread uses this track). Also prepares
        every device in the chain. */
    void prepare(double sampleRate, int maxBlockSize);
    bool isPrepared() const noexcept { return preparedBlockSize > 0; }

    /** Message thread, with the devices already prepared (AudioEngine::installChain does both under
        the engine's prepare lock). */
    void setChain(std::shared_ptr<const DeviceChainLayout> chain);
    /** Message thread. The chain published last (may be null). */
    std::shared_ptr<const DeviceChainLayout> chain() const { return chains.published(); }
    /** Message thread. Frees chains and tunings the audio thread no longer uses. */
    void collectGarbage();
    /** Message thread. Chain versions not yet freed (for tests). */
    std::size_t chainVersions() const noexcept { return chains.versionCount(); }

    /** Any thread. The last note-on the chain received, or -1 (for the play panel's readout). */
    int lastNoteOn() const noexcept { return lastNote.load(std::memory_order_relaxed); }

    /** Message thread. Peak levels since the previous call (and resets them). */
    std::array<float, 2> takePeaks() noexcept;

    /** Any thread. The clip launcher state for the UI. */
    TrackPlayState playState() const noexcept { return published.load(); }

    /** Tests: sees every event the instrument gets. Set before rendering; nullptr removes it. */
    void setEventTap(TrackEventTap* tap) noexcept { eventTap.store(tap); }

    // --- Audio thread -----------------------------------------------------------------------------
    TrackSessionState& session() noexcept { return state; }

    /** Starts collecting events for a chunk that begins at absolute sample chunkStart. */
    MidiEventSink& beginChunk(std::int64_t chunkStart) noexcept LATTICE_NONBLOCKING;
    MidiEventSink& events() noexcept { return sink; }

    static constexpr int maxReturns = 12;
    static constexpr double maxCompensationSeconds = 0.5;

    /** Stage 1 (may run on a worker thread): the device chain over the chunk's events. input is the
        chain's audio input (a return's sends, the master's mix) or null for silence (MIDI tracks).
        Leaves the chain's output in chainOutput(), and that output delayed by `compensation`
        samples (delay compensation) in output(). Touches only this processor's own state. */
    void renderChain(int numSamples, const DeviceTimeInfo& time, const float* const* input,
                     int compensation) noexcept LATTICE_NONBLOCKING;

    /** What one mix() call sends where. */
    struct Sends
    {
        const float* gains = nullptr;   // per return, linear (0 = off)
        const bool* preFader = nullptr; // per return
        float* const* buses = nullptr;  // per return: two channels, added to
        int count = 0;
        bool audible = true;            // false: the sends are muted too
    };

    /** Stage 2 (audio thread, in a fixed order): fader and pan (ramped towards the targets), sends,
        meters; adds the result into busLeft / busRight. */
    void mix(int numSamples, float targetLeft, float targetRight, const Sends& sends, float* busLeft,
             float* busRight) noexcept LATTICE_NONBLOCKING;

    /** After renderChain(): the chain's output (before compensation) and after compensation. */
    const float* const* chainOutput() const noexcept { return chainChannels; }
    const float* const* output() const noexcept { return outputChannels; }
    /** After renderChain(): the audio after the instruments (the "pre-effects" sidechain point),
        if setPreFxTapWanted(true). */
    const float* const* preFxTap() const noexcept { return tapChannels; }
    /** Message thread: whether some device sidechains from this track before its effects. */
    void setPreFxTapWanted(bool wanted) noexcept { tapWanted.store(wanted, std::memory_order_relaxed); }

    /** The latency of the chain in use (audio thread) or published (message thread). */
    int latencySamples() const noexcept { return publishedLatency.load(std::memory_order_relaxed); }

    void publishPlayState(const TrackPlayState& s) noexcept LATTICE_NONBLOCKING { published.store(s); }
    std::uint32_t droppedEvents() const noexcept { return dropped.load(std::memory_order_relaxed); }

private:
    using ChainVersion = SettledShared<DeviceChainLayout>::Version;
    enum class ChainFade : std::uint8_t
    {
        Steady,
        Out,
        In
    };

    /** Audio thread: makes `next` the chain in use, releasing held notes if the MIDI path changed. */
    void switchChain(const ChainVersion* next) noexcept LATTICE_NONBLOCKING;
    void applyChainFade(int numSamples) noexcept LATTICE_NONBLOCKING;

    const std::int64_t id;
    TrackTuning trackTuning;
    SettledShared<DeviceChainLayout> chains;
    TrackSessionState state;

    // Audio thread: the chain in use and the switch between chains.
    const ChainVersion* activeChain = nullptr;
    ChainFade chainFade = ChainFade::In;
    float chainGain = 0.0f;
    float chainFadeStep = 1.0f;
    ChainRunner runner;

    std::vector<TimedMidi> eventStorage;
    MidiEventSink sink;
    juce::AudioBuffer<float> buffer;     // the chain's output
    juce::AudioBuffer<float> delayed;    // after compensation
    juce::AudioBuffer<float> tapBuffer;  // the pre-effects tap
    float* chainChannels[2] = {nullptr, nullptr};
    float* outputChannels[2] = {nullptr, nullptr};
    float* tapChannels[2] = {nullptr, nullptr};
    std::atomic<bool> tapWanted{false};
    std::atomic<int> publishedLatency{0};
    dsp::DelayLine compensationLines[2];
    juce::SmoothedValue<float> gainLeft;
    juce::SmoothedValue<float> gainRight;
    std::array<float, maxReturns> sendGains{};
    int preparedBlockSize = 0;

    std::atomic<TrackEventTap*> eventTap{nullptr};
    std::atomic<int> lastNote{-1};
    alignas(64) std::array<std::atomic<float>, 2> peaks{};
    std::atomic<std::uint32_t> dropped{0};
    Seqlock<TrackPlayState> published;
};

} // namespace lattice
