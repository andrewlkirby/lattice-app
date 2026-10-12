#pragma once

#include "core/AudioCallbackStats.h"
#include "core/RealtimeCheck.h"
#include "devices/test/TuningTestSynth.h"
#include "engine/MidiInputQueue.h"
#include "engine/TestToneSource.h"
#include "tuning/TrackTuning.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

namespace lattice
{

/** Owns the audio device and the single audio-callback entry point. Renders the test tone and,
    since M1, one "track": live MIDI -> TuningTestSynth tuned by a TrackTuning. In M2 it grows into
    the real engine (docs/03-m2-engine-session.md).

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
    TrackTuning& trackTuning() noexcept { return tuning; }
    TuningTestSynth& synth() noexcept { return testSynth; }

    /** Live MIDI into the track (any thread except the audio thread). Enabled MIDI input devices
        are routed here automatically. */
    MidiInputQueue& midiInput() noexcept { return midiQueue; }
    AudioCallbackStats& callbackStats() noexcept { return stats; }

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

private:
    void handleIncomingMidiMessage(juce::MidiInput* source, const juce::MidiMessage& message) override;

    juce::String openDeviceNow(const juce::XmlElement* savedState);
    void waitForOpenThread();

    juce::AudioDeviceManager deviceManager;
    TestToneSource tone;
    AudioCallbackStats stats;
    TrackTuning tuning;
    TuningTestSynth testSynth{tuning};
    MidiInputQueue midiQueue;

    // Sized in prepare(); never resized on the audio thread.
    std::vector<float> scratch;
    std::vector<float> toneScratch;
    std::array<TimedMidi, maxEventsPerBlock> events{};
    double sampleRate = 0.0;
    bool callbackRegistered = false;

    std::thread openThread;
    std::atomic<bool> opening{false};
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR(AudioEngine)
};

} // namespace lattice
