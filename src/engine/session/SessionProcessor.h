#pragma once

#include "core/RealtimeCheck.h"
#include "core/RtFifo.h"
#include "core/Seqlock.h"
#include "engine/EngineCommands.h"
#include "engine/EngineSnapshot.h"
#include "engine/Metronome.h"
#include "devices/dsp/DelayLine.h"
#include "engine/Transport.h"
#include "engine/WorkerPool.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <limits>
#include <vector>

namespace lattice
{

/** Transport and session flags as the UI sees them, published once per chunk. */
struct TransportState
{
    bool playing = false;
    bool sessionRecord = false;
    bool stopAllQueued = false;
    std::int16_t timeSigNum = 4;
    std::int16_t timeSigDen = 4;
    std::int32_t barOriginIndex = 0;
    double beat = 0.0;
    double tempo = 120.0;
    double barOrigin = 0.0;
    std::int64_t sample = 0;
};

/** The session engine on the audio thread (docs/03-m2-engine-session.md, parts 2b-2d): applies
    commands, schedules quantized launches, stops, scene changes, follow actions and recordings at
    exact samples, renders every track's clip events, runs the instruments, and mixes.

    Each chunk is walked from one scheduled action to the next ("block splitting"): clip events up to
    the action's sample come from the old state, events from that sample on from the new one. Every
    action's sample is computed from beats and the transport, so results do not depend on block
    sizes. Nothing here allocates, locks or blocks. */
class SessionProcessor final : private SidechainLookup
{
public:
    explicit SessionProcessor(SpscFifo<EngineEvent>& eventsToMessageThread);

    /** Not on the audio thread. */
    void prepare(double sampleRate, int maxBlockSize);

    /** Audio thread. numSamples must not exceed the prepared block size. Fills left/right. */
    void process(const EngineSnapshot* snapshot, const EngineCommand* commands, int numCommands,
                 const TimedMidi* liveInput, int numLiveInput, int numSamples, float* left, float* right,
                 const WorkerPool* pool = nullptr) noexcept LATTICE_NONBLOCKING;

    /** Any thread. */
    TransportState transportState() const noexcept { return published.load(); }
    /** Message thread. Master peak levels since the previous call. */
    std::array<float, 2> takeMasterPeaks() noexcept;

    /** Audio thread / tests (when nothing is processing). */
    const Transport& transport() const noexcept { return clock; }

    /** Message thread. Peak levels of a return since the previous call. */
    std::array<float, 2> takeReturnPeaks(int returnIndex) noexcept;
    std::int64_t currentSample() const noexcept { return now; }

private:
    using Queued = TrackPlayState::Queued;
    static constexpr double never = TrackSessionState::never;

    struct SceneChange
    {
        double beat = never;
        double tempo = 0.0;
        int timeSigNum = 0;
        int timeSigDen = 0;
    };

    void syncWithSnapshot(const EngineSnapshot& snapshot, std::int64_t sample) noexcept;
    void applyCommand(const EngineSnapshot& snapshot, const EngineCommand& command,
                      std::int64_t sample) noexcept;
    void routeLiveInput(const EngineSnapshot& snapshot, const TimedMidi* input, int numInput,
                        std::int64_t sample) noexcept;

    /** The earliest sample (>= from) at which something is scheduled, or `limit`. */
    std::int64_t nextScheduledSample(const EngineSnapshot& snapshot, std::int64_t from,
                                     std::int64_t limit) const noexcept;
    /** Runs everything scheduled at or before `sample`. */
    void fireDue(const EngineSnapshot& snapshot, std::int64_t sample) noexcept;
    bool isDue(double beat, std::int64_t sample) const noexcept;

    void ensurePlaying(std::int64_t sample) noexcept;
    double quantizedBeat(Quantization q, double beat) const noexcept;
    double clipQuantizedBeat(const ClipData& clip, double beat) const noexcept;
    void setPending(const TrackSnapshot& track, const TrackSessionState::Pending& pending) noexcept;
    void queueStop(const TrackSnapshot& track, double beat) noexcept;
    void stopAllNow(const EngineSnapshot& snapshot, std::int64_t sample) noexcept;

    void executePending(const EngineSnapshot& snapshot, const TrackSnapshot& track,
                        std::int64_t sample) noexcept;
    void launchClip(const TrackSnapshot& track, const ClipData& clip, double beat,
                    std::int64_t sample) noexcept;
    void stopTrack(const TrackSnapshot& track, std::int64_t sample) noexcept;
    void fireFollowAction(const EngineSnapshot& snapshot, const TrackSnapshot& track,
                          std::int64_t sample) noexcept;

    /** Ends a new-clip recording at sample: open-ended ones get their length rounded up to whole
        bars and loop from then on. */
    void finishRecording(const TrackSnapshot& track, std::int64_t sample) noexcept;
    void updateCapture(const TrackSnapshot& track, std::int64_t sample) noexcept;
    void endCapture(const TrackSnapshot& track, std::int64_t sample, double finalLength) noexcept;

    void publish(const EngineSnapshot& snapshot) noexcept;
    /** The mixer: every track's chain (by level), the sends, the returns, the master's chain. */
    void renderMix(const EngineSnapshot& snapshot, const DeviceTimeInfo& time, int numSamples, float* left,
                   float* right, const WorkerPool* pool) noexcept LATTICE_NONBLOCKING;
    const float* const* channels(const SidechainRoute& route) const noexcept override;
    void post(const EngineEvent& event) noexcept { eventsOut.push(event); }
    double nextRandom() noexcept;

    SpscFifo<EngineEvent>& eventsOut;
    Transport clock;
    Metronome metronome;
    std::int64_t now = 0;
    double sampleRate = 48000.0;
    int maxBlock = 0;

    std::uint64_t lastGeneration = 0;
    double lastSnapshotTempo = std::numeric_limits<double>::quiet_NaN();
    int lastSnapshotSigNum = 0;
    int lastSnapshotSigDen = 0;
    Quantization globalQuantization = Quantization::Bar1;

    SceneChange pendingScene;
    bool sessionRecord = false;
    double stopAllBeat = never;
    std::uint64_t randomState = 0x9e3779b97f4a7c15ull;
    double freeRunningBeat = 0.0; // devices' beat while the transport is stopped

    std::vector<float> metronomeBuffer;
    // The mixer's buses (sized in prepare)
    juce::AudioBuffer<float> returnInputs; // two channels per return
    juce::AudioBuffer<float> trackBus;
    juce::AudioBuffer<float> returnBus;
    juce::AudioBuffer<float> masterInput;
    std::array<float*, 2 * TrackProcessor::maxReturns> returnInputPointers{};
    std::array<dsp::DelayLine, 2> masterInputDelay;
    std::array<juce::SmoothedValue<float>, TrackProcessor::maxReturns * 2> returnGains;
    std::array<std::array<std::atomic<float>, 2>, TrackProcessor::maxReturns> returnPeaks{};
    const EngineSnapshot* rendering = nullptr; // the snapshot being mixed (for sidechain lookups)
    juce::SmoothedValue<float> masterLeft;
    juce::SmoothedValue<float> masterRight;
    alignas(64) std::array<std::atomic<float>, 2> masterPeaks{};
    Seqlock<TransportState> published;
};

} // namespace lattice
