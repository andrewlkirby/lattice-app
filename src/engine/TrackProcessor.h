#pragma once

#include "core/RealtimeCheck.h"
#include "core/Seqlock.h"
#include "devices/test/TestSynthProcessor.h"
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

/** One track in the engine: instrument, tuning, playback state, mixer gain and meters
    (docs/03-m2-engine-session.md, part 2b). Created by the SnapshotBuilder on the message thread and
    kept per track ID across snapshots, so rebuilding a snapshot never recreates the instrument or
    cuts off its sound. Shared by the snapshots that list it; freed on the message thread. */
class TrackProcessor
{
public:
    static constexpr int maxEventsPerChunk = 4096;
    static constexpr double gainRampSeconds = 0.02;

    explicit TrackProcessor(std::int64_t trackId);
    ~TrackProcessor();

    TrackProcessor(const TrackProcessor&) = delete;
    TrackProcessor& operator=(const TrackProcessor&) = delete;

    std::int64_t trackId() const noexcept { return id; }
    TrackTuning& tuning() noexcept { return trackTuning; }
    juce::AudioProcessor& instrument() noexcept { return *synth; }

    /** Not on the audio thread (and not while the audio thread uses this track). */
    void prepare(double sampleRate, int maxBlockSize);
    bool isPrepared() const noexcept { return preparedBlockSize > 0; }

    /** Message thread. Last note-on that started a voice, or -1 (for the play panel's readout). */
    int lastNoteOn() const noexcept { return synth->synth().lastNoteOn(); }

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

    /** Runs the instrument over the chunk's events, applies gain and pan (smoothed towards
        targetLeft/targetRight), meters the result, and adds it to the master buffers. */
    void render(int numSamples, float targetLeft, float targetRight, float* masterLeft,
                float* masterRight) noexcept LATTICE_NONBLOCKING;

    void publishPlayState(const TrackPlayState& s) noexcept LATTICE_NONBLOCKING { published.store(s); }
    std::uint32_t droppedEvents() const noexcept { return dropped.load(std::memory_order_relaxed); }

private:
    const std::int64_t id;
    TrackTuning trackTuning;
    std::unique_ptr<TestSynthProcessor> synth;
    TrackSessionState state;

    std::vector<TimedMidi> eventStorage;
    MidiEventSink sink;
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> buffer;
    juce::SmoothedValue<float> gainLeft;
    juce::SmoothedValue<float> gainRight;
    int preparedBlockSize = 0;

    std::atomic<TrackEventTap*> eventTap{nullptr};
    alignas(64) std::array<std::atomic<float>, 2> peaks{};
    std::atomic<std::uint32_t> dropped{0};
    Seqlock<TrackPlayState> published;
};

} // namespace lattice
