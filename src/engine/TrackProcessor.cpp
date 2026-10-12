#include "engine/TrackProcessor.h"

#include <algorithm>

namespace lattice
{

TrackProcessor::TrackProcessor(std::int64_t trackId)
    : id(trackId), synth(std::make_unique<TestSynthProcessor>(trackTuning))
{
}

TrackProcessor::~TrackProcessor() = default;

void TrackProcessor::prepare(double sampleRate, int maxBlockSize)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    maxBlockSize = std::max(maxBlockSize, 1);
    trackTuning.setSampleRate(sampleRate);
    synth->setPlayConfigDetails(0, 2, sampleRate, maxBlockSize);
    synth->prepareToPlay(sampleRate, maxBlockSize);

    eventStorage.assign(static_cast<std::size_t>(maxEventsPerChunk), TimedMidi{});
    sink = MidiEventSink{eventStorage.data(), maxEventsPerChunk, 0, 0, 0};
    midi.clear();
    midi.ensureSize(static_cast<std::size_t>(maxEventsPerChunk) * 16);
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

void TrackProcessor::render(int numSamples, float targetLeft, float targetRight, float* masterLeft,
                            float* masterRight) noexcept
{
    numSamples = std::min(numSamples, preparedBlockSize);
    if (numSamples <= 0)
        return;

    if (sink.dropped > 0)
    {
        dropped.fetch_add(sink.dropped, std::memory_order_relaxed);
        sink.dropped = 0;
    }

    // Events can come out of the scheduler with equal offsets in any channel order, but always in
    // non-decreasing time order; MidiBuffer keeps that order.
    midi.clear();
    auto* tap = eventTap.load(std::memory_order_relaxed);
    for (int i = 0; i < sink.count; ++i)
    {
        const auto& e = sink.events[i];
        const int offset = std::clamp(e.sampleOffset, 0, numSamples - 1);
        midi.addEvent(e.bytes, e.size, offset);
        if (tap != nullptr)
            tap->onTrackEvent(id, sink.chunkStart + offset, e);
    }

    float* const channels[2] = {buffer.getWritePointer(0), buffer.getWritePointer(1)};
    juce::AudioBuffer<float> view(channels, 2, numSamples);
    synth->processBlock(view, midi);

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
