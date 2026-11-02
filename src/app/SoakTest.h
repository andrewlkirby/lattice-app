#pragma once

#include "engine/AudioEngine.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <juce_events/juce_events.h>

#include <functional>

namespace lattice
{

/** Dropout soak (docs/09-performance-charter.md §3, M0 gate): plays the test tone at a given buffer
    size for a fixed time and counts callback overruns and device xruns. Started with
    `Lattice --soak-seconds 600 --buffer-size 64`. Appends a progress line every minute, a dropout
    line within 0.25 s of each new overrun or xrun (with what setContext describes at that moment),
    and one result line to <userdata>/logs/soak.log, then calls onFinished. Message thread only. */
class SoakTest final : private juce::Timer
{
public:
    struct Options
    {
        double seconds = 600.0;
        int bufferSize = 64;     // the smallest size the device allows at or above this is used
        double sampleRate = 0.0; // 0: keep the device's current rate
        juce::String deviceType;  // empty: keep the opened device's type
        juce::String deviceName;  // empty: that type's current/default device (ASIO: the driver name)
        float levelDb = -40.0f;   // quiet, but not silent: the normal render path runs
        double warmupSeconds = 2.0; // excluded from the counts (device start-up)
        /** The UI stress test (docs/05-m4-ui.md, Tests 11): instead of the test tone, the set in
            setFile plays while UiStress works the window. */
        bool uiStress = false;
        juce::String setFile;
        /** A WAV file of what the device plays, from the start line to the result (24-bit). */
        juce::String recordFile;
    };

    /** Reads --soak-seconds, --buffer-size, --sample-rate, --device-type, --device-name,
        --soak-level-db, --ui-stress, --set and --record. Returns false if --soak-seconds is absent. */
    static bool parseCommandLine(const juce::String& commandLine, Options& options);

    SoakTest(AudioEngine& engine, Options options, juce::File logFile, std::function<void(bool passed)> onFinished);
    ~SoakTest() override;

    /** Call once the device is open. */
    void start();
    /** What the app is doing, for the dropout lines (the UI stress test's step). */
    void setContext(std::function<juce::String()> describe) { context = std::move(describe); }

private:
    void timerCallback() override;
    void writeLine(const juce::String& kind);
    void finish();
    void startRecording();
    void stopRecording();

    /** What the device plays, into the WAV file (lock-free on the audio thread). */
    struct Recorder final : AudioEngine::OutputTap
    {
        std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter> writer;
        void write(const float* const* channels, int numChannels, int numSamples) noexcept override
        {
            if (numChannels >= 2)
                writer->write(channels, numSamples);
        }
    };
    juce::TimeSliceThread recordThread{"Lattice soak recording"};
    std::unique_ptr<Recorder> recorder;

    AudioEngine& engine;
    Options options;
    juce::File logFile;
    std::function<void(bool)> onFinished;
    std::function<juce::String()> context;

    juce::String setupError;
    juce::String availableBufferSizes;
    int requestedBuffer = 0;
    double startTime = 0.0;
    int xrunBaseline = 0;
    bool measuring = false;
    int lastProgressMinute = 0;
    std::uint64_t overrunsSeen = 0;
    int xrunsSeen = 0;
};

} // namespace lattice
