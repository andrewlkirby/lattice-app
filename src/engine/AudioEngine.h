#pragma once

#include "core/AudioCallbackStats.h"
#include "core/LatencyStats.h"
#include "core/RealtimeCheck.h"
#include "core/RtFifo.h"
#include "core/RtShared.h"
#include "engine/EngineCommands.h"
#include "engine/EngineSnapshot.h"
#include "engine/MidiInputQueue.h"
#include "engine/WorkerPool.h"
#include "engine/PreviewPlayer.h"
#include "engine/TestToneSource.h"
#include "devices/racks/RackLayout.h"
#include "engine/session/SessionProcessor.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace lattice
{

/** Owns the audio device and the single audio-callback entry point, and hosts the session engine
    (docs/03-m2-engine-session.md): the message thread publishes EngineSnapshots and sends
    EngineCommands; the audio thread renders the tracks, the metronome and the test tone, and sends
    EngineEvents back.

    Threading: device methods are message-thread only. process() is the audio thread's work and can
    also be driven directly (offline) by tests after prepare(). */
class AudioEngine final : public juce::AudioIODeviceCallback, private juce::MidiInputCallback
{
public:
    static constexpr int numOutputChannels = 2;

    AudioEngine();
    ~AudioEngine() override;

    juce::AudioDeviceManager& getDeviceManager() noexcept { return deviceManager; }
    TestToneSource& testTone() noexcept { return tone; }
    /** The browser's preview bus, mixed after the master. */
    PreviewPlayer& preview() noexcept { return previewPlayer; }

    /** Live MIDI to the armed tracks (any thread except the audio thread). Enabled MIDI input
        devices are routed here automatically. */
    MidiInputQueue& midiInput() noexcept { return midiQueue; }
    /** The source number live MIDI from this input carries (by the input's identifier; 1 to 255,
        given out on first use and kept for the session). Any thread except the audio thread. */
    std::uint8_t midiSourceFor(const juce::String& inputIdentifier);

    /** MIDI Map's inputs (docs/05-m4-ui.md, 4g): what they send is kept for the message thread
        (takeRemoteMidi). An input open only for that (its Track switch off) is in notForTracks:
        its notes don't reach the tracks. Message thread. */
    void setRemoteInputs(const juce::StringArray& remote, const juce::StringArray& notForTracks);
    struct RemoteMidi
    {
        juce::String input;
        juce::MidiMessage message;
    };
    /** Message thread: what the remote inputs sent since the last call, oldest first. */
    std::vector<RemoteMidi> takeRemoteMidi();
    AudioCallbackStats& callbackStats() noexcept { return stats; }
    /** Input latency probe: MIDI arrival to the end of the callback that plays it. */
    LatencyStats& inputLatency() noexcept { return latency; }

    // --- Session engine -------------------------------------------------------------------------
    /** Message thread. Makes snapshot the one the audio thread uses from its next block. */
    void publishSnapshot(std::shared_ptr<const EngineSnapshot> snapshot);
    /** Message thread. The snapshot published last. */
    const std::shared_ptr<const EngineSnapshot>& currentSnapshot() const noexcept { return snapshots.published(); }
    /** Message thread. Frees snapshots the audio thread has finished with. */
    void collectGarbage()
    {
        snapshots.collect();
        workers.collect();
        previewPlayer.collectGarbage();
    }

    /** Any thread except the audio thread. Returns false if the queue was full. */
    bool sendCommand(const EngineCommand& command)
    {
        noteActivity();
        return commands.push(command);
    }

    /** Something the message thread may soon have to follow: a command, a new snapshot, a
        preview, MIDI coming in. What polls the engine (SessionController, ui::EngineStateFeed) does
        so less often while nothing happens, and wakes on this: each listener's update is
        triggered. Any thread but the audio thread. */
    void addActivityListener(juce::AsyncUpdater* listener);

    /** Something that takes what the device plays, each callback, on the audio thread (the soak's
        recording: SoakTest). It must not block or allocate. */
    struct OutputTap
    {
        virtual ~OutputTap() = default;
        virtual void write(const float* const* channels, int numChannels, int numSamples) noexcept = 0;
    };
    /** From the next callback on (nullptr: none). Message thread; returns once the audio thread
        no longer uses the tap before, so it can be deleted. */
    void setOutputTap(OutputTap* tap);
    void removeActivityListener(const juce::AsyncUpdater* listener);
    void noteActivity();
    /** Message thread (the single consumer). Events from the audio thread. */
    SpscFifo<EngineEvent>& engineEvents() noexcept { return events; }
    /** Transport state and master peaks (any thread, see SessionProcessor). */
    SessionProcessor& session() noexcept { return sessionProcessor; }

    /** Message thread. Parallel track processing: 0 workers processes everything on the audio
        thread (the default, and what tests use unless they ask); WorkerPool::defaultWorkers() uses
        every physical core. Safe while audio runs: the old pool goes once the audio thread is done
        with it. */
    void setWorkerThreads(int count);
    int workerThreads() const noexcept { return workerCount; }
    /** Message thread. Each thread's time in track work so far, in nanoseconds: [0] the audio
        thread, then each worker (WorkerPool::busyNanoseconds()). Empty with no workers; starts
        over when the worker count changes. */
    std::vector<std::uint64_t> workerBusyNanoseconds() const;

    /** Message thread. A new track processor, already prepared for the current device settings,
        and re-prepared whenever they change. */
    std::shared_ptr<TrackProcessor> createTrackProcessor(std::int64_t trackId);

    /** Message thread. Prepares the new devices for the current device settings and publishes the
        chain to the track, both under the prepare lock, so a device restart on another thread can
        never see a half-prepared device. Fills in the chain's latency once its devices are prepared. */
    void installChain(TrackProcessor& track, const std::vector<DevicePtr>& newDevices,
                      const std::vector<std::shared_ptr<RackChainState>>& newRackChains,
                      std::shared_ptr<DeviceChainLayout> chain);

    /** Message thread. Opens the saved device setup if there is one, otherwise the first device
        type in platform::preferredAudioDeviceTypes() that opens. Returns an error message, or an
        empty string on success. */
    juce::String openDevice(const juce::XmlElement* savedState);

    /** Message thread. Like openDevice(), but slow driver work happens on a background thread so the
        UI is never blocked (WASAPI exclusive mode on some drivers takes over a second to open).
        Device types are scanned here first, because some create message-thread windows. Types that
        need the message thread (ASIO) are opened synchronously. onOpened(error) is called on the
        message thread when done. Until then, nothing else may use getDeviceManager(). */
    void openDeviceAsync(std::unique_ptr<juce::XmlElement> savedState,
                         std::function<void(const juce::String& error)> onOpened);

    /** True while openDeviceAsync() is still working. */
    bool isOpening() const noexcept { return opening.load(); }

    /** Message thread. Stops the callback and closes the device. */
    void closeDevice();

    /** Not on the audio thread. Allocates everything process() will need. */
    void prepare(double sampleRate, int maxBlockSize);

    /** Audio thread (or a test). Fills every non-null output channel with numSamples samples. */
    void process(float* const* outputs, int numOutputs, int numSamples) noexcept LATTICE_NONBLOCKING;

    // juce::AudioIODeviceCallback
    void audioDeviceIOCallbackWithContext(const float* const* inputs, int numInputs, float* const* outputs,
                                          int numOutputs, int numSamples,
                                          const juce::AudioIODeviceCallbackContext& context) override;
    void audioDeviceAboutToStart(juce::AudioIODevice* device) override;
    void audioDeviceStopped() override;

    static constexpr int maxEventsPerBlock = 512;
    static constexpr int maxCommandsPerBlock = 256;
    static constexpr int commandQueueSize = 1024;
    static constexpr int eventQueueSize = 16384;

private:
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;

    std::mutex midiSourcesLock;
    std::map<juce::String, std::uint8_t> midiSources;

    juce::String openDeviceNow(const juce::XmlElement* savedState);
    void waitForOpenThread();

    juce::AudioDeviceManager deviceManager;
    TestToneSource tone;
    PreviewPlayer previewPlayer;
    AudioCallbackStats stats;
    LatencyStats latency;
    std::mutex remoteLock; // MIDI threads and the message thread; never the audio thread
    std::mutex activityLock; // the same threads
    std::vector<juce::AsyncUpdater*> activityListeners;
    std::atomic<OutputTap*> outputTap{nullptr};
    std::atomic<int> outputTapUsers{0}; // callbacks between taking the tap and finishing with it
    juce::StringArray remoteInputs, nonTrackInputs;
    std::vector<RemoteMidi> remoteInbox;
    MidiInputQueue midiQueue;
    MpscQueue<EngineCommand> commands{commandQueueSize};
    SpscFifo<EngineEvent> events{eventQueueSize};
    RtShared<EngineSnapshot> snapshots;
    RtShared<WorkerPool> workers;
    int workerCount = 0;
    SessionProcessor sessionProcessor{events};

    // Track processors to re-prepare when the device settings change.
    std::mutex processorsMutex;
    std::vector<std::weak_ptr<TrackProcessor>> processors;
    double preparedSampleRate = 48000.0;
    int preparedBlockSize = 512;

    // Sized in prepare(); never resized on the audio thread.
    std::vector<float> left;
    std::vector<float> right;
    std::vector<float> toneScratch;
    std::array<TimedMidi, maxEventsPerBlock> liveEvents{};
    std::array<double, maxEventsPerBlock> liveArrivals{};
    std::array<EngineCommand, maxCommandsPerBlock> commandScratch{};
    double sampleRate = 0.0;
    bool callbackRegistered = false;

    std::thread openThread;
    std::atomic<bool> opening{false};
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};

} // namespace lattice
