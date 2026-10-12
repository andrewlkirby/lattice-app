#include "engine/AudioEngine.h"

#include "core/Profiling.h"
#include "platform/Platform.h"

#include <algorithm>
#include <chrono>

namespace lattice
{

AudioEngine::AudioEngine() = default;

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
        deviceManager.removeAudioCallback(this);
        callbackRegistered = false;
    }
    deviceManager.closeAudioDevice();
}

void AudioEngine::prepare(double newSampleRate, int maxBlockSize)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    sampleRate = newSampleRate;
    scratch.assign(static_cast<std::size_t>(std::max(maxBlockSize, 1)), 0.0f);
    tone.prepare(newSampleRate);
    stats.reset();
}

void AudioEngine::process(float* const* outputs, int numOutputs, int numSamples) noexcept
{
    const int chunkSize = static_cast<int>(scratch.size());
    if (sampleRate <= 0.0 || chunkSize == 0)
    {
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
                std::fill(outputs[ch], outputs[ch] + numSamples, 0.0f);
        return;
    }

    // Some drivers deliver blocks larger than the size they announced; render in chunks rather
    // than ever resizing on this thread.
    for (int offset = 0; offset < numSamples; offset += chunkSize)
    {
        const int n = std::min(chunkSize, numSamples - offset);
        tone.render(scratch.data(), n);
        for (int ch = 0; ch < numOutputs; ++ch)
            if (outputs[ch] != nullptr)
                std::copy(scratch.data(), scratch.data() + n, outputs[ch] + offset);
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

} // namespace lattice
