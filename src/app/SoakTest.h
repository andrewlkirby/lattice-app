#pragma once

#include "engine/AudioEngine.h"

#include <juce_events/juce_events.h>

#include <functional>

namespace lattice
{

/** Dropout soak (docs/09-performance-charter.md §3, M0 gate): plays the test tone at a given buffer
    size for a fixed time and counts callback overruns and device xruns. Started with
    `Lattice --soak-seconds 600 --buffer-size 64`. Appends a progress line every minute and one
    result line to <userdata>/logs/soak.log, then calls onFinished. Message thread only. */
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
    };

    /** Reads --soak-seconds, --buffer-size, --sample-rate, --device-type, --device-name and --soak-level-db.
        Returns false if --soak-seconds is absent. */
    static bool parseCommandLine(const juce::String& commandLine, Options& options);

    SoakTest(AudioEngine& engine, Options options, juce::File logFile, std::function<void(bool passed)> onFinished);
    ~SoakTest() override;

    /** Call once the device is open. */
    void start();

private:
    void timerCallback() override;
    void writeLine(const juce::String& kind);
    void finish();

    AudioEngine& engine;
    Options options;
    juce::File logFile;
    std::function<void(bool)> onFinished;

    juce::String setupError;
    juce::String availableBufferSizes;
    int requestedBuffer = 0;
    double startTime = 0.0;
    int xrunBaseline = 0;
    bool measuring = false;
    int lastProgressMinute = 0;
};

} // namespace lattice
