#include "app/SoakTest.h"

namespace lattice
{
namespace
{
double nowSeconds()
{
    return juce::Time::getMillisecondCounterHiRes() * 0.001;
}

juce::String pct(double fraction)
{
    return juce::String(fraction * 100.0, 1) + "%";
}
} // namespace

bool SoakTest::parseCommandLine(const juce::String& commandLine, Options& options)
{
    const auto args = juce::StringArray::fromTokens(commandLine, true);
    const auto valueOf = [&](const char* name) -> juce::String {
        const int i = args.indexOf(name);
        return i >= 0 && i + 1 < args.size() ? args[i + 1].unquoted() : juce::String();
    };

    const auto seconds = valueOf("--soak-seconds");
    if (seconds.isEmpty())
        return false;

    options.seconds = juce::jmax(1.0, seconds.getDoubleValue());
    if (const auto v = valueOf("--buffer-size"); v.isNotEmpty())
        options.bufferSize = juce::jmax(1, v.getIntValue());
    if (const auto v = valueOf("--sample-rate"); v.isNotEmpty())
        options.sampleRate = v.getDoubleValue();
    if (const auto v = valueOf("--device-type"); v.isNotEmpty())
        options.deviceType = v;
    if (const auto v = valueOf("--device-name"); v.isNotEmpty())
        options.deviceName = v;
    if (const auto v = valueOf("--soak-level-db"); v.isNotEmpty())
        options.levelDb = v.getFloatValue();
    return true;
}

SoakTest::SoakTest(AudioEngine& e, Options o, juce::File log, std::function<void(bool)> done)
    : engine(e), options(std::move(o)), logFile(std::move(log)), onFinished(std::move(done))
{
}

SoakTest::~SoakTest()
{
    stopTimer();
}

void SoakTest::start()
{
    auto& dm = engine.getDeviceManager();

    if (options.deviceType.isNotEmpty() && dm.getCurrentAudioDeviceType() != options.deviceType)
        dm.setCurrentAudioDeviceType(options.deviceType, true);

    auto setup = dm.getAudioDeviceSetup();
    requestedBuffer = options.bufferSize;

    if (options.deviceName.isNotEmpty() && setup.outputDeviceName != options.deviceName)
    {
        setup.outputDeviceName = options.deviceName;
        setup.inputDeviceName = {};
        setupError = dm.setAudioDeviceSetup(setup, false);
        setup = dm.getAudioDeviceSetup();
    }

    if (options.sampleRate > 0.0 && setupError.isEmpty())
    {
        // Change the rate first: the buffer sizes a device allows can depend on it.
        setup.sampleRate = options.sampleRate;
        setupError = dm.setAudioDeviceSetup(setup, false);
        setup = dm.getAudioDeviceSetup();
    }

    if (auto* device = dm.getCurrentAudioDevice(); device != nullptr && setupError.isEmpty())
    {
        // Devices only accept sizes from their own list (WASAPI exclusive: whole device periods).
        // Use the smallest one at or above the request; if none is that large, the largest.
        const auto sizes = device->getAvailableBufferSizes();
        int chosen = sizes.isEmpty() ? options.bufferSize : sizes.getLast();
        for (const int size : sizes)
        {
            if (size >= options.bufferSize)
            {
                chosen = size;
                break;
            }
        }

        juce::StringArray names;
        for (const int size : sizes)
            names.add(juce::String(size));
        constexpr int maxListed = 8;
        if (names.size() > maxListed)
        {
            const auto last = names[names.size() - 1];
            names.removeRange(maxListed, names.size());
            names.add("..." + last);
        }
        availableBufferSizes = names.joinIntoString(",");

        setup.bufferSize = chosen;
        setupError = dm.setAudioDeviceSetup(setup, false);
    }

    engine.testTone().setGain(juce::Decibels::decibelsToGain(options.levelDb, -100.0f));
    engine.testTone().setEnabled(true);

    startTime = nowSeconds();
    startTimer(250);
}

void SoakTest::timerCallback()
{
    const double elapsed = nowSeconds() - startTime;

    if (!measuring)
    {
        if (elapsed < options.warmupSeconds)
            return;
        engine.callbackStats().reset();
        xrunBaseline = engine.getDeviceManager().getXRunCount();
        startTime = nowSeconds();
        measuring = true;
        writeLine("start");
        return;
    }

    if (const int minute = static_cast<int>(elapsed / 60.0); minute > lastProgressMinute)
    {
        lastProgressMinute = minute;
        writeLine("progress");
    }

    if (elapsed >= options.seconds)
        finish();
}

void SoakTest::writeLine(const juce::String& kind)
{
    auto& dm = engine.getDeviceManager();
    const auto s = engine.callbackStats().summary();
    const int xruns = dm.getXRunCount() - xrunBaseline;

    juce::String line = juce::Time::getCurrentTime().toISO8601(true);
    line << " | " << kind << " | elapsed=" << juce::String(measuring ? nowSeconds() - startTime : 0.0, 1);

    if (auto* device = dm.getCurrentAudioDevice())
    {
        const double sr = device->getCurrentSampleRate();
        const int block = device->getCurrentBufferSizeSamples();
        line << " | type=" << dm.getCurrentAudioDeviceType() << " | device=" << device->getName()
             << " | rate=" << juce::String(sr, 0) << " | buffer=" << block << " (requested " << requestedBuffer
             << ", available " << availableBufferSizes << ")"
             << " | buffer ms=" << juce::String(1000.0 * block / sr, 2)
             << " | output latency ms=" << juce::String(1000.0 * device->getOutputLatencyInSamples() / sr, 2);

        // Which physical outputs are in use (ASIO4ALL names its channels after the device).
        juce::StringArray active;
        const auto channelNames = device->getOutputChannelNames();
        const auto activeBits = device->getActiveOutputChannels();
        for (int ch = 0; ch < channelNames.size(); ++ch)
            if (activeBits[ch])
                active.add(channelNames[ch]);
        line << " | outputs=" << active.joinIntoString(" + ");
    }
    else
    {
        line << " | no device";
    }

    line << " | callbacks=" << juce::String(static_cast<juce::int64>(s.callbacks))
         << " | overruns=" << juce::String(static_cast<juce::int64>(s.overruns)) << " | xruns=" << xruns
         << " | p50=" << pct(s.p50) << " | p99=" << pct(s.p99) << " | p99.9=" << pct(s.p999)
         << " | max=" << pct(s.max);
    if (setupError.isNotEmpty())
        line << " | setup error=" << setupError;

    if (kind == "result")
    {
        const bool passed = setupError.isEmpty() && dm.getCurrentAudioDevice() != nullptr && s.callbacks > 0
                         && s.overruns == 0 && xruns == 0;
        line << " | " << (passed ? "PASS" : "FAIL");
    }

    logFile.getParentDirectory().createDirectory();
    logFile.appendText(line + "\n");
}

void SoakTest::finish()
{
    stopTimer();
    engine.testTone().setEnabled(false);

    const auto s = engine.callbackStats().summary();
    const int xruns = engine.getDeviceManager().getXRunCount() - xrunBaseline;
    const bool passed = setupError.isEmpty() && engine.getDeviceManager().getCurrentAudioDevice() != nullptr
                     && s.callbacks > 0 && s.overruns == 0 && xruns == 0;
    writeLine("result");

    if (onFinished)
        onFinished(passed);
}

} // namespace lattice
