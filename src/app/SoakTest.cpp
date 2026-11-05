#include "app/SoakTest.h"

#include "control/surface/PadPlayer.h"
#include "control/surface/Presets.h"

#include <random>

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

juce::String ms(double value)
{
    return juce::String(value, 2);
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
    options.uiStress = args.contains("--ui-stress");
    options.setFile = valueOf("--set");
    options.recordFile = valueOf("--record");
    options.pads = args.contains("--pads");
    return true;
}

SoakTest::SoakTest(AudioEngine& e, Options o, juce::File log, std::function<void(bool)> done)
    : engine(e), options(std::move(o)), logFile(std::move(log)), onFinished(std::move(done))
{
}

SoakTest::~SoakTest()
{
    stopTimer();
    stopPads();
    stopRecording();
}

void SoakTest::startPads()
{
    if (!options.pads || padThread.joinable())
        return;
    padsRunning = true;
    padThread = std::thread([this] {
        juce::Thread::setCurrentThreadName("Lattice soak pads");
        // An 8x8 grid's pads, each a note: pressed every 20 to 80 ms, held 10 to 200 ms, up to
        // four at once, as a player's fingers.
        const auto caps = control::presets::gridOnly(8, 8);
        control::PadPlayer player(engine, caps);
        auto map = control::PadNoteMap::empty(caps);
        map.source = engine.midiSourceFor("lattice.soak.pads");
        for (int i = 0; i < caps.numPads(); ++i)
        {
            auto& pad = map.pads[static_cast<std::size_t>(i)];
            pad.kind = control::PadNoteMap::Pad::Kind::note;
            pad.note = static_cast<std::uint8_t>(36 + i);
        }
        player.publish(std::make_shared<const control::PadNoteMap>(std::move(map)));
        std::mt19937 rng(17);
        struct Held
        {
            int pad;
            double until;
        };
        std::vector<Held> held;
        double nextPress = nowSeconds();
        while (padsRunning.load())
        {
            const double now = nowSeconds();
            for (auto it = held.begin(); it != held.end();)
                if (it->until <= now)
                {
                    auto up = control::SurfaceInput::padUp(it->pad / 8, it->pad % 8);
                    player.handle(up);
                    it = held.erase(it);
                }
                else
                    ++it;
            if (now >= nextPress && held.size() < 4)
            {
                const int pad = static_cast<int>(rng() % 64);
                if (std::none_of(held.begin(), held.end(), [&](const Held& h) { return h.pad == pad; }))
                {
                    auto down = control::SurfaceInput::padDown(pad / 8, pad % 8, 0.4f + 0.6f * static_cast<float>(rng() % 100) / 100.0f);
                    player.handle(down);
                    held.push_back({pad, now + 0.01 + 0.19 * static_cast<double>(rng() % 1000) / 1000.0});
                    padPresses.fetch_add(1, std::memory_order_relaxed);
                }
                nextPress = now + 0.02 + 0.06 * static_cast<double>(rng() % 1000) / 1000.0;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        player.releaseAll();
    });
}

void SoakTest::stopPads()
{
    padsRunning = false;
    if (padThread.joinable())
        padThread.join();
}

juce::String SoakTest::padsReport() const
{
    // From a press arriving to the block that plays it handed to the device (the input probe),
    // then the device's own output latency on top: pad to sound.
    const auto s = engine.inputLatency().summary();
    juce::String line;
    line << " | pads: presses=" << juce::String(static_cast<juce::int64>(padPresses.load())) << " events=" << juce::String(static_cast<juce::int64>(s.count))
         << " to the device ms p50=" << ms(s.p50Ms) << " p99=" << ms(s.p99Ms) << " max=" << ms(s.maxMs);
    if (auto* device = engine.getDeviceManager().getCurrentAudioDevice())
    {
        const double out = 1000.0 * device->getOutputLatencyInSamples() / device->getCurrentSampleRate();
        line << " | pad to sound ms (with the output latency) p50=" << ms(s.p50Ms + out) << " p99=" << ms(s.p99Ms + out)
             << " max=" << ms(s.maxMs + out);
    }
    return line;
}

void SoakTest::startRecording()
{
    auto* device = engine.getDeviceManager().getCurrentAudioDevice();
    if (options.recordFile.isEmpty() || device == nullptr)
        return;
    const auto file = juce::File::getCurrentWorkingDirectory().getChildFile(options.recordFile);
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> stream = file.createOutputStream();
    if (stream == nullptr)
        return;
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                  .withSampleRate(device->getCurrentSampleRate())
                                                  .withNumChannels(2)
                                                  .withBitsPerSample(24));
    if (writer == nullptr)
        return;
    recordThread.startThread();
    recorder = std::make_unique<Recorder>();
    // A second of audio in the FIFO: the background thread writes it out.
    recorder->writer = std::make_unique<juce::AudioFormatWriter::ThreadedWriter>(
        writer.release(), recordThread, juce::roundToInt(device->getCurrentSampleRate()));
    engine.setOutputTap(recorder.get());
}

void SoakTest::stopRecording()
{
    if (recorder == nullptr)
        return;
    engine.setOutputTap(nullptr);
    recorder.reset(); // the writer writes what's left and closes the file
    recordThread.stopThread(2000);
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

    if (!options.uiStress) // the UI stress test plays a set instead
    {
        engine.testTone().setGain(juce::Decibels::decibelsToGain(options.levelDb, -100.0f));
        engine.testTone().setEnabled(true);
    }

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
        overrunsSeen = 0;
        xrunsSeen = 0;
        startRecording(); // from the start line's time
        engine.inputLatency().reset();
        if (onMeasuring)
            onMeasuring();
        startPads();
        writeLine("start");
        return;
    }

    // A line for each new dropout, so that one can be traced to what was happening.
    const auto overruns = engine.callbackStats().summary().overruns;
    const int xruns = engine.getDeviceManager().getXRunCount() - xrunBaseline;
    if (overruns != overrunsSeen || xruns != xrunsSeen)
    {
        overrunsSeen = overruns;
        xrunsSeen = xruns;
        writeLine("dropout");
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
    line << " | " << kind << (options.uiStress ? " (ui stress: " + options.setFile + ")" : juce::String()) << " | elapsed=" << juce::String(measuring ? nowSeconds() - startTime : 0.0, 1);

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
    if (kind == "dropout" && context)
        line << " | during=" << context();
    if (measuring && (kind == "progress" || kind == "result"))
    {
        if (options.pads)
            line << padsReport();
        if (report)
            line << report();
    }

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
    stopPads();
    stopRecording();
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
