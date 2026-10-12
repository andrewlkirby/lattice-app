#pragma once

#include "core/AudioCallbackStats.h"
#include "core/LatencyStats.h"
#include "core/RealtimeCheck.h"
#include "core/RtFifo.h"
#include "core/RtShared.h"
#include "engine/EngineCommands.h"
#include "engine/EngineSnapshot.h"
#include "engine/MidiInputQueue.h"
#include "engine/TestToneSource.h"
#include "engine/session/SessionProcessor.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
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

    /** Live MIDI to the armed tracks (any thread except the audio thread). Enabled MIDI input
        devices are routed here automatically. */
    MidiInputQueue& midiInput() noexcept { return midiQueue; }
    AudioCallbackStats& callbackStats() noexcept { return stats; }
    /** Input latency probe: MIDI arrival to the end of the callback that plays it. */
    LatencyStats& inputLatency() noexcept { return latency; }

    // --- Session engine -------------------------------------------------------------------------
    /** Message thread. Makes snapshot the one the audio thread uses from its next block. */
    void publishSnapshot(std::shared_ptr<const EngineSnapshot> snapshot);
    /** Message thread. The snapshot published last. */
    const std::shared_ptr<const EngineSnapshot>& currentSnapshot() const noexcept { return snapshots.published(); }
    /** Message thread. Frees snapshots the audio thread has finished with. */
    void collectGarbage() { snapshots.collect(); }

    /** Any thread except the audio thread. Returns false if the queue was full. */
    bool sendCommand(const EngineCommand& command) { return commands.push(command); }
    /** Message thread (the single consumer). Events from the audio thread. */
    SpscFifo<EngineEvent>& engineEvents() noexcept { return events; }
    /** Transport state and master peaks (any thread, see SessionProcessor). */
    SessionProcessor& session() noexcept { return sessionProcessor; }

    /** Message thread. A new track processor, already prepared for the current device settings,
        and re-prepared whenever they change. */
    std::shared_ptr<TrackProcessor> createTrackProcessor(std::int64_t trackId);

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

    juce::String openDeviceNow(const juce::XmlElement* savedState);
    void waitForOpenThread();

    juce::AudioDeviceManager deviceManager;
    TestToneSource tone;
    AudioCallbackStats stats;
    LatencyStats latency;
    MidiInputQueue midiQueue;
    MpscQueue<EngineCommand> commands{commandQueueSize};
    SpscFifo<EngineEvent> events{eventQueueSize};
    RtShared<EngineSnapshot> snapshots;
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
