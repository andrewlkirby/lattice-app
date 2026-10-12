#include "engine/TrackProcessor.h"

#include <algorithm>

namespace lattice
{

TrackProcessor::TrackProcessor(std::int64_t trackId) : id(trackId)
{
}

TrackProcessor::~TrackProcessor() = default;

void TrackProcessor::prepare(double sampleRate, int maxBlockSize)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    maxBlockSize = std::max(maxBlockSize, 1);
    trackTuning.setSampleRate(sampleRate);

    // The audio thread isn't running this track now. Re-prepare the devices; their latencies may
    // depend on the sample rate, so the chain is republished with them, and nothing older is in use.
    if (const auto chain = chains.published())
    {
        for (const auto& device : chain->devices)
            device->prepareDevice(sampleRate, maxBlockSize);
        auto updated = std::make_shared<DeviceChainLayout>(*chain);
        updated->finalise();
        chains.publish(std::move(updated));
    }
    chains.resetReader();
    activeChain = nullptr;
    chainFade = ChainFade::In;
    chainGain = 0.0f;
    chainFadeStep = static_cast<float>(1.0 / std::max(1.0, chainFadeSeconds * sampleRate));
    runner.prepare(maxEventsPerChunk);

    eventStorage.assign(static_cast<std::size_t>(maxEventsPerChunk), TimedMidi{});
    sink = MidiEventSink{eventStorage.data(), maxEventsPerChunk, 0, 0, 0};
    buffer.setSize(2, maxBlockSize, false, true, false);
    buffer.clear();

    gainLeft.reset(sampleRate, gainRampSeconds);
    gainRight.reset(sampleRate, gainRampSeconds);
    gainLeft.setCurrentAndTargetValue(0.0f);
    gainRight.setCurrentAndTargetValue(0.0f);

    // A device restart ends whatever was playing; the scheduler starts from a clean state.
    state = TrackSessionState{};
    preparedBlockSize = maxBlockSize;
}

void TrackProcessor::setChain(std::shared_ptr<const DeviceChainLayout> chain)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    chains.publish(std::move(chain));
}

void TrackProcessor::collectGarbage()
{
    chains.collect();
    trackTuning.collectGarbage();
}

std::array<float, 2> TrackProcessor::takePeaks() noexcept
{
    return {peaks[0].exchange(0.0f, std::memory_order_relaxed),
            peaks[1].exchange(0.0f, std::memory_order_relaxed)};
}

MidiEventSink& TrackProcessor::beginChunk(std::int64_t chunkStart) noexcept
{
    sink.count = 0;
    sink.chunkStart = chunkStart;
    return sink;
}

void TrackProcessor::switchChain(const ChainVersion* next) noexcept
{
    const DeviceChainLayout* before = activeChain != nullptr ? activeChain->object.get() : nullptr;
    const DeviceChainLayout* after = next != nullptr ? next->object.get() : nullptr;
    activeChain = next;
    chains.settle(next);
    chainFade = ChainFade::In;

    // A different MIDI path (a MIDI effect or instrument added, removed or moved) could leave notes
    // hanging in devices that stay: release them.
    if (before != nullptr && after != nullptr && before->midiPath != after->midiPath)
        for (const auto* device : after->midiPath)
            const_cast<StudioDevice*>(device)->releaseAllNotes();
}

void TrackProcessor::applyChainFade(int numSamples) noexcept
{
    if (chainFade == ChainFade::Steady)
        return;

    float* left = buffer.getWritePointer(0);
    float* right = buffer.getWritePointer(1);
    float g = chainGain;
    const float step = chainFade == ChainFade::In ? chainFadeStep : -chainFadeStep;
    for (int i = 0; i < numSamples; ++i)
    {
        g = juce::jlimit(0.0f, 1.0f, g + step);
        left[i] *= g;
        right[i] *= g;
    }
    chainGain = g;
    if (chainFade == ChainFade::In && g >= 1.0f)
        chainFade = ChainFade::Steady;
}

void TrackProcessor::render(int numSamples, const DeviceTimeInfo& time, float targetLeft, float targetRight,
                            float* masterLeft, float* masterRight) noexcept
{
    numSamples = std::min(numSamples, preparedBlockSize);
    if (numSamples <= 0)
        return;

    if (sink.dropped > 0)
    {
        dropped.fetch_add(sink.dropped, std::memory_order_relaxed);
        sink.dropped = 0;
    }

    // The chain's MIDI input: the scheduler's events, in non-decreasing time order, kept in the block.
    auto* tap = eventTap.load(std::memory_order_relaxed);
    for (int i = 0; i < sink.count; ++i)
    {
        auto& e = sink.events[i];
        e.sampleOffset = std::clamp(e.sampleOffset, 0, numSamples - 1);
        if (tap != nullptr)
            tap->onTrackEvent(id, sink.chunkStart + e.sampleOffset, e);
        if (isNoteOn(e))
            lastNote.store(e.bytes[1], std::memory_order_relaxed);
    }
    MidiBlock input{sink.events, sink.capacity, sink.count, 0};

    // A newer chain: fade the current one out first (or switch at once if nothing is audible).
    const auto* latest = chains.latest();
    if (latest != activeChain)
    {
        if (activeChain == nullptr || chainGain <= 0.0f)
            switchChain(latest);
        else
            chainFade = ChainFade::Out;
    }

    float* const channels[2] = {buffer.getWritePointer(0), buffer.getWritePointer(1)};
    std::fill_n(channels[0], numSamples, 0.0f);
    std::fill_n(channels[1], numSamples, 0.0f);
    if (activeChain != nullptr && activeChain->object != nullptr)
        if (const auto lost = runner.process(*activeChain->object, channels, numSamples, input, time); lost > 0)
            dropped.fetch_add(lost, std::memory_order_relaxed);

    applyChainFade(numSamples);
    if (chainFade == ChainFade::Out && chainGain <= 0.0f)
        switchChain(latest); // faded out: the new chain fades in from the next block

    gainLeft.setTargetValue(targetLeft);
    gainRight.setTargetValue(targetRight);
    float* left = channels[0];
    float* right = channels[1];
    if (gainLeft.isSmoothing() || gainRight.isSmoothing())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            left[i] *= gainLeft.getNextValue();
            right[i] *= gainRight.getNextValue();
        }
    }
    else
    {
        juce::FloatVectorOperations::multiply(left, gainLeft.getCurrentValue(), numSamples);
        juce::FloatVectorOperations::multiply(right, gainRight.getCurrentValue(), numSamples);
    }

    const auto rangeLeft = juce::FloatVectorOperations::findMinAndMax(left, numSamples);
    const auto rangeRight = juce::FloatVectorOperations::findMinAndMax(right, numSamples);
    const float peakLeft = std::max(-rangeLeft.getStart(), rangeLeft.getEnd());
    const float peakRight = std::max(-rangeRight.getStart(), rangeRight.getEnd());
    if (peakLeft > peaks[0].load(std::memory_order_relaxed))
        peaks[0].store(peakLeft, std::memory_order_relaxed);
    if (peakRight > peaks[1].load(std::memory_order_relaxed))
        peaks[1].store(peakRight, std::memory_order_relaxed);

    juce::FloatVectorOperations::add(masterLeft, left, numSamples);
    juce::FloatVectorOperations::add(masterRight, right, numSamples);
}

} // namespace lattice
