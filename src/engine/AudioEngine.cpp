#include "engine/AudioEngine.h"

#include "core/Profiling.h"
#include "platform/Platform.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <thread>
#include <utility>

namespace lattice
{

AudioEngine::AudioEngine() = default;

std::vector<std::uint64_t> AudioEngine::workerBusyNanoseconds() const
{
    const auto& pool = workers.published();
    return pool != nullptr ? pool->busyNanoseconds() : std::vector<std::uint64_t>{};
}

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
    previewPlayer.prepare(newSampleRate);
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
    noteActivity();
}

void AudioEngine::setOutputTap(OutputTap* tap)
{
    JUCE_ASSERT_MESSAGE_THREAD
    outputTap.store(tap);
    // A callback that took the old tap counted itself first: once none is counted, none has it.
    while (outputTapUsers.load() != 0)
        std::this_thread::yield();
}

void AudioEngine::addActivityListener(juce::AsyncUpdater* listener)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    const std::scoped_lock lock(activityLock);
    if (std::find(activityListeners.begin(), activityListeners.end(), listener) == activityListeners.end())
        activityListeners.push_back(listener);
}

void AudioEngine::removeActivityListener(const juce::AsyncUpdater* listener)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    const std::scoped_lock lock(activityLock);
    std::erase(activityListeners, listener);
}

void AudioEngine::noteActivity()
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    const std::scoped_lock lock(activityLock);
    for (auto* listener : activityListeners)
        listener->triggerAsyncUpdate();
}

void AudioEngine::process(float* const* outputs, int numOutputs, int numSamples, double callbackSeconds) noexcept
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
    const int numLive = midiQueue.popInto(liveEvents.data(), maxEventsPerBlock, liveArrivals.data());
    const double reference = callbackSeconds >= 0.0 ? callbackSeconds : MidiInputQueue::nowSeconds();
    placeLiveInput(numLive, numSamples, callbackSeconds >= 0.0 ? reference : -1.0);
    int nextLive = 0;

    // Some drivers deliver blocks larger than the size they announced; render in chunks rather
    // than ever resizing on this thread.
    for (int offset = 0; offset < numSamples; offset += chunkSize)
    {
        const int n = std::min(chunkSize, numSamples - offset);
        {
            const RtShared<EngineSnapshot>::ReadScope scope(snapshots);
            const RtShared<WorkerPool>::ReadScope pool(workers);
            // This chunk's live input, at offsets from its start (they're in time order).
            int chunkLive = 0;
            while (nextLive + chunkLive < numLive && liveEvents[static_cast<std::size_t>(nextLive + chunkLive)].sampleOffset < offset + n)
                liveEvents[static_cast<std::size_t>(nextLive + chunkLive++)].sampleOffset -= offset;
            sessionProcessor.process(scope.get(), commandScratch.data(), numCommands, liveEvents.data() + nextLive,
                                     chunkLive, n, left.data(), right.data(), pool.get());
            nextLive += chunkLive;
        }
        numCommands = 0; // commands belong to the first chunk

        tone.render(toneScratch.data(), n);
        juce::FloatVectorOperations::add(left.data(), toneScratch.data(), n);
        juce::FloatVectorOperations::add(right.data(), toneScratch.data(), n);

        // The preview bus, after the master. A synced preview waits for the next bar.
        double toNextBar = -1.0;
        if (const auto& clock = sessionProcessor.transport(); clock.isPlaying())
        {
            const auto start = sessionProcessor.currentSample() - n;
            const double beat = clock.beatAt(start);
            const double origin = sessionProcessor.transportState().barOrigin;
            const double perBar = clock.beatsPerBar();
            const double nextBar = origin + std::ceil((beat - origin) / perBar - 1.0e-9) * perBar;
            toNextBar = static_cast<double>(clock.sampleAtBeat(nextBar) - start);
        }
        previewPlayer.render(left.data(), right.data(), n, toNextBar);

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

    // From arrival to the moment its sample plays, counted from the callback's start (the driver's
    // output latency comes on top). With the device's clock that is one block for every event.
    for (int i = 0; i < numLive; ++i)
        latency.record((reference - liveArrivals[static_cast<std::size_t>(i)] + livePlaced[static_cast<std::size_t>(i)] / sampleRate) * 1000.0);
}

void AudioEngine::placeLiveInput(int numLive, int numSamples, double callbackSeconds) noexcept
{
    // A message that arrived one block's time before this callback plays at the block's start, one
    // that arrived just now at its end: each plays exactly one block after it arrived, so the
    // delay is constant whatever the callback's own timing. Older ones (a late callback) play at
    // the start. Without the device's clock (offline), everything plays at the start.
    for (int i = 0; i < numLive; ++i)
    {
        const auto at = static_cast<std::size_t>(i);
        int offset = 0;
        if (callbackSeconds >= 0.0)
        {
            const double age = (callbackSeconds - liveArrivals[at]) * sampleRate;
            offset = std::clamp(numSamples - static_cast<int>(std::lround(age)), 0, numSamples - 1);
        }
        liveEvents[at].sampleOffset = offset;
        livePlaced[at] = offset;
    }
    // Producers on several threads can interleave: put them in time order (stable, in place; nearly
    // always already sorted).
    for (int i = 1; i < numLive; ++i)
    {
        const auto e = liveEvents[static_cast<std::size_t>(i)];
        const double arrival = liveArrivals[static_cast<std::size_t>(i)];
        int j = i;
        for (; j > 0 && liveEvents[static_cast<std::size_t>(j - 1)].sampleOffset > e.sampleOffset; --j)
        {
            liveEvents[static_cast<std::size_t>(j)] = liveEvents[static_cast<std::size_t>(j - 1)];
            liveArrivals[static_cast<std::size_t>(j)] = liveArrivals[static_cast<std::size_t>(j - 1)];
            livePlaced[static_cast<std::size_t>(j)] = livePlaced[static_cast<std::size_t>(j - 1)];
        }
        liveEvents[static_cast<std::size_t>(j)] = e;
        liveArrivals[static_cast<std::size_t>(j)] = arrival;
        livePlaced[static_cast<std::size_t>(j)] = e.sampleOffset;
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
    process(outputs, numOutputs, numSamples, std::chrono::duration<double>(start.time_since_epoch()).count());
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    // Counted before the tap is read, so setOutputTap() can tell when the old one is free.
    outputTapUsers.fetch_add(1);
    if (auto* tap = outputTap.load())
        tap->write(outputs, numOutputs, numSamples);
    outputTapUsers.fetch_sub(1);

    stats.recordCallback(elapsed, static_cast<double>(numSamples) / sampleRate);
}

void AudioEngine::audioDeviceAboutToStart(juce::AudioIODevice* device)
{
    prepare(device->getCurrentSampleRate(), device->getCurrentBufferSizeSamples());
}

void AudioEngine::audioDeviceStopped()
{
}

std::uint8_t AudioEngine::midiSourceFor(const juce::String& inputIdentifier)
{
    const std::scoped_lock lock(midiSourcesLock);
    if (const auto it = midiSources.find(inputIdentifier); it != midiSources.end())
        return it->second;
    // 255 inputs in one session is far beyond any setup; past it, they share the last number.
    const auto next = static_cast<std::uint8_t>(juce::jmin<std::size_t>(255, midiSources.size() + 1));
    midiSources[inputIdentifier] = next;
    return next;
}

void AudioEngine::setRemoteInputs(const juce::StringArray& remote, const juce::StringArray& notForTracks)
{
    const std::scoped_lock lock(remoteLock);
    remoteInputs = remote;
    nonTrackInputs = notForTracks;
}

std::vector<AudioEngine::RemoteMidi> AudioEngine::takeRemoteMidi()
{
    const std::scoped_lock lock(remoteLock);
    return std::exchange(remoteInbox, {});
}

void AudioEngine::setSurfaceSink(const juce::String& inputIdentifier, SurfaceSink* sink, bool notesToTracks)
{
    // Taking the lock waits out a MIDI thread inside the old sink.
    const std::scoped_lock lock(remoteLock);
    if (sink == nullptr)
        surfaces.erase(inputIdentifier);
    else
        surfaces[inputIdentifier] = {sink, notesToTracks};
}

bool AudioEngine::isSurfaceInput(const juce::String& inputIdentifier) const
{
    const std::scoped_lock lock(remoteLock);
    return surfaces.contains(inputIdentifier);
}

void AudioEngine::handleIncomingMidiMessage(juce::MidiInput* input, const juce::MidiMessage& message)
{
    if (input != nullptr)
        receiveMidi(input->getIdentifier(), message);
    else
    {
        midiQueue.push(message, computerKeyboardSource);
        noteActivity();
    }
}

void AudioEngine::receiveMidi(const juce::String& id, const juce::MidiMessage& message)
{
    // A MIDI thread, not the audio thread: a short lock is fine here.
    bool forTracks = true;
    {
        const std::scoped_lock lock(remoteLock);
        bool remote = remoteInputs.contains(id);
        forTracks = !nonTrackInputs.contains(id);
        if (const auto it = surfaces.find(id); it != surfaces.end())
        {
            // The surface first; what isn't its own goes to MIDI Map, and notes to the tracks only
            // if the surface passes them on.
            if (it->second.sink->receive(message, -1.0))
            {
                noteActivity();
                return;
            }
            remote = true;
            forTracks = it->second.notesToTracks;
        }
        if (remote && remoteInbox.size() < 4096) // a stalled UI can't grow it without end
            remoteInbox.push_back({id, message});
    }
    if (forTracks)
        midiQueue.push(message, midiSourceFor(id));
    noteActivity();
}

} // namespace lattice
