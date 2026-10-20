#include "engine/AudioEngine.h"

#include "core/Profiling.h"
#include "platform/Platform.h"

#include <algorithm>
#include <chrono>

namespace lattice
{

AudioEngine::AudioEngine() = default;

void AudioEngine::setWorkerThreads(int count)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    count = std::max(0, count);
    if (count == workerCount && workers.published() != nullptr)
        return;
    workerCount = count;
    workers.publish(count > 0 ? std::make_shared<const WorkerPool>(count) : nullptr);
}

AudioEngine::~AudioEngine()
{
    alive->store(false);
    waitForOpenThread();
    closeDevice();
}

void AudioEngine::waitForOpenThread()
{
    if (openThread.joinable())
        openThread.join();
}

juce::String AudioEngine::openDevice(const juce::XmlElement* savedState)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    waitForOpenThread();
    closeDevice();
    deviceManager.getAvailableDeviceTypes();
    return openDeviceNow(savedState);
}

void AudioEngine::openDeviceAsync(std::unique_ptr<juce::XmlElement> savedState,
                                  std::function<void(const juce::String&)> onOpened)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    JUCE_ASSERT_MESSAGE_THREAD
    waitForOpenThread();
    closeDevice();

    // Creating the device types must happen here: WASAPI's device-change detector is a window.
    deviceManager.getAvailableDeviceTypes();

    const auto savedType = savedState != nullptr ? savedState->getStringAttribute("deviceType") : juce::String();
    const auto firstChoice = savedType.isNotEmpty() ? savedType : platform::preferredAudioDeviceTypes()[0];
    if (platform::audioDeviceTypeNeedsMessageThread(firstChoice))
    {
        const auto error = openDeviceNow(savedState.get());
        juce::MessageManager::callAsync([token = alive, onOpened, error] {
            if (token->load() && onOpened)
                onOpened(error);
        });
        return;
    }

    opening.store(true);
    openThread = std::thread([this, state = std::move(savedState), onOpened = std::move(onOpened)] {
        juce::Thread::setCurrentThreadName("Lattice audio device open");
        juce::String error;
        {
            const platform::ScopedAudioDeviceThread deviceThread;
            error = openDeviceNow(state.get());
        }
        opening.store(false);
        juce::MessageManager::callAsync([this, token = alive, onOpened, error] {
            if (!token->load())
                return;
            waitForOpenThread();
            if (onOpened)
                onOpened(error);
        });
    });
}

juce::String AudioEngine::openDeviceNow(const juce::XmlElement* savedState)
{
    juce::String error;
    bool opened = false;

    if (savedState != nullptr)
    {
        error = deviceManager.initialise(0, numOutputChannels, savedState, true);
        opened = error.isEmpty() && deviceManager.getCurrentAudioDevice() != nullptr;
    }

    if (!opened)
    {
        // No saved setup (or it failed): try the platform's preferred device types in order, each
        // with its default output device. (JUCE opens nothing if the device name is left empty.)
        const auto& available = deviceManager.getAvailableDeviceTypes(); // already scanned
        for (const auto& typeName : platform::preferredAudioDeviceTypes())
        {
            const auto it = std::find_if(available.begin(), available.end(),
                                         [&](auto* t) { return t->getTypeName() == typeName; });
            if (it == available.end())
                continue;

            const auto outputs = (*it)->getDeviceNames(false);
            if (outputs.isEmpty())
                continue;
            const auto& defaultOutput = outputs[juce::jmax(0, (*it)->getDefaultDeviceIndex(false))];

            juce::XmlElement setup("DEVICESETUP");
            setup.setAttribute("deviceType", typeName);
            setup.setAttribute("audioOutputDeviceName", defaultOutput);
            error = deviceManager.initialise(0, numOutputChannels, &setup, false);
            opened = error.isEmpty() && deviceManager.getCurrentAudioDevice() != nullptr;
            if (opened)
                break;
        }
    }

    if (!opened)
    {
        error = deviceManager.initialiseWithDefaultDevices(0, numOutputChannels);
        opened = error.isEmpty() && deviceManager.getCurrentAudioDevice() != nullptr;
    }

    deviceManager.addAudioCallback(this);
    deviceManager.addMidiInputDeviceCallback({}, this); // every enabled MIDI input
    callbackRegistered = true;

    if (!opened && error.isEmpty())
        error = "No audio output device could be opened.";
    return error;
}

void AudioEngine::closeDevice()
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    if (callbackRegistered)
    {
        deviceManager.removeMidiInputDeviceCallback({}, this);
        deviceManager.removeAudioCallback(this);
        callbackRegistered = false;
    }
    deviceManager.closeAudioDevice();
}

void AudioEngine::prepare(double newSampleRate, int maxBlockSize)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    sampleRate = newSampleRate;
    const auto block = static_cast<std::size_t>(std::max(maxBlockSize, 1));
    left.assign(block, 0.0f);
    right.assign(block, 0.0f);
    toneScratch.assign(block, 0.0f);
    tone.prepare(newSampleRate);
    sessionProcessor.prepare(newSampleRate, static_cast<int>(block));
    stats.reset();

    const std::scoped_lock lock(processorsMutex);
    preparedSampleRate = newSampleRate;
    preparedBlockSize = static_cast<int>(block);
    processors.erase(std::remove_if(processors.begin(), processors.end(), [](const auto& p) { return p.expired(); }),
                     processors.end());
    for (const auto& weak : processors)
        if (const auto p = weak.lock())
            p->prepare(newSampleRate, static_cast<int>(block));
}

std::shared_ptr<TrackProcessor> AudioEngine::createTrackProcessor(std::int64_t trackId)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    auto processor = std::make_shared<TrackProcessor>(trackId);
    const std::scoped_lock lock(processorsMutex);
    processor->prepare(preparedSampleRate, preparedBlockSize);
    if (processors.size() > 64)
        processors.erase(std::remove_if(processors.begin(), processors.end(), [](const auto& p) { return p.expired(); }),
                         processors.end());
    processors.push_back(processor);
    return processor;
}

void AudioEngine::installChain(TrackProcessor& track, const std::vector<DevicePtr>& newDevices,
                               const std::vector<std::shared_ptr<RackChainState>>& newRackChains,
                               std::shared_ptr<DeviceChainLayout> chain)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    const std::scoped_lock lock(processorsMutex);
    for (const auto& device : newDevices)
        device->prepareDevice(preparedSampleRate, preparedBlockSize);
    for (const auto& state : newRackChains)
        state->prepare(preparedBlockSize);
    chain->finalise();
    track.setChain(std::move(chain));
}

void AudioEngine::publishSnapshot(std::shared_ptr<const EngineSnapshot> snapshot)
{
    snapshots.publish(std::move(snapshot));
}

void AudioEngine::process(float* const* outputs, int numOutputs, int numSamples) noexcept
{
    // Denormals flush to zero on this thread (FTZ/DAZ), so decaying tails never slow processing.
    const juce::ScopedNoDenormals noDenormals;
    const int chunkSize = static_cast<int>(left.size());
    if (sampleRate <= 0.0 || chunkSize == 0)
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
                std::fill(outputs[ch], outputs[ch] + numSamples, 0.0f);
        return;
    }

    // Commands first, then the snapshot: a command is always pushed after the snapshot it refers
    // to was published, so the snapshot read below is at least that new.
    int numCommands = commands.popInto(commandScratch.data(), maxCommandsPerBlock);
    // Live MIDI that arrived since the last block plays at the start of this one.
    int numLive = midiQueue.popInto(liveEvents.data(), maxEventsPerBlock, liveArrivals.data());
    const int liveReceived = numLive;

    // Some drivers deliver blocks larger than the size they announced; render in chunks rather
    // than ever resizing on this thread.
    for (int offset = 0; offset < numSamples; offset += chunkSize)
    {
        const int n = std::min(chunkSize, numSamples - offset);
        {
            const RtShared<EngineSnapshot>::ReadScope scope(snapshots);
            const RtShared<WorkerPool>::ReadScope pool(workers);
            sessionProcessor.process(scope.get(), commandScratch.data(), numCommands, liveEvents.data(), numLive, n,
                                     left.data(), right.data(), pool.get());
        }
        numCommands = 0; // commands and live input belong to the first chunk
        numLive = 0;

        tone.render(toneScratch.data(), n);
        juce::FloatVectorOperations::add(left.data(), toneScratch.data(), n);
        juce::FloatVectorOperations::add(right.data(), toneScratch.data(), n);

        for (int ch = 0; ch < numOutputs; ++ch)
        {
            float* out = outputs[ch];
            if (out == nullptr)
                continue;
            out += offset;
            if (numOutputs == 1)
            {
                for (int i = 0; i < n; ++i)
                    out[i] = 0.5f * (left[static_cast<std::size_t>(i)] + right[static_cast<std::size_t>(i)]);
            }
            else if (ch < 2)
            {
                std::copy_n(ch == 0 ? left.data() : right.data(), n, out);
            }
            else
            {
                std::fill(out, out + n, 0.0f);
            }
        }
    }

    if (liveReceived > 0)
    {
        const double end = MidiInputQueue::nowSeconds();
        for (int i = 0; i < liveReceived; ++i)
            latency.record((end - liveArrivals[static_cast<std::size_t>(i)]) * 1000.0);
    }
}

void AudioEngine::audioDeviceIOCallbackWithContext(const float* const* inputs, int numInputs,
                                                   float* const* outputs, int numOutputs, int numSamples,
                                                   const juce::AudioIODeviceCallbackContext& context)
{
    juce::ignoreUnused(inputs, numInputs, context);
    const rt::ScopedAudioThread audioThread;
    LATTICE_ZONE("audio callback");

    const auto start = std::chrono::steady_clock::now();
    process(outputs, numOutputs, numSamples);
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    stats.recordCallback(elapsed, static_cast<double>(numSamples) / sampleRate);
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    prepare(device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
}

void AudioEngine::audioDeviceStopped()
{
}

void AudioEngine::handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage& message)
{
    midiQueue.push(message);
}

} // namespace lattice
