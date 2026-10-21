#include "engine/TrackProcessor.h"

#include "devices/racks/Rack.h"

#include <algorithm>

namespace lattice
{

namespace
{
/** The same device instances in the same order, racks included: switching between the two needs no
    fade (e.g. only a sidechain route changed). Audio thread: compares pointers, allocates nothing. */
bool sameDevicesIn(const DeviceChainLayout* a, const DeviceChainLayout* b) noexcept
{
    return a != nullptr && b != nullptr && a->devices == b->devices && a->racks == b->racks;
}
} // namespace

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
        chain->forEachDevice([&](StudioDevice& device) { device.prepareDevice(sampleRate, maxBlockSize); });
        forEachRack(*chain, [&](StudioDevice&, RackLayout& rack) {
            for (auto& c : rack.chains)
                c.state->prepare(maxBlockSize);
        });
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
    delayed.setSize(2, maxBlockSize, false, true, false);
    delayed.clear();
    tapBuffer.setSize(2, maxBlockSize, false, true, false);
    tapBuffer.clear();
    groupBuffer.setSize(2, maxBlockSize, false, true, false);
    groupBuffer.clear();
    sendBuffer.setSize(4, maxBlockSize, false, true, false);
    sendBuffer.clear();
    for (int ch = 0; ch < 2; ++ch)
    {
        chainChannels[ch] = buffer.getWritePointer(ch);
        outputChannels[ch] = delayed.getWritePointer(ch);
        tapChannels[ch] = tapBuffer.getWritePointer(ch);
        groupChannels[ch] = groupBuffer.getWritePointer(ch);
        compensationLines[ch].prepare(static_cast<int>(maxCompensationSeconds * sampleRate) + 4);
        sendLines[ch].prepare(static_cast<int>(maxCompensationSeconds * sampleRate) + 4);
    }
    for (int ch = 0; ch < 4; ++ch)
        sendChannels[ch] = sendBuffer.getWritePointer(ch);
    sendGains.fill(0.0f);

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
    publishedLatency.store(chain != nullptr ? chain->latencySamples : 0, std::memory_order_relaxed);
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

    // Every rack in the new tree takes its part of the layout.
    if (after != nullptr)
        forEachRack(*after, [](StudioDevice& device, const RackLayout& rack) {
            static_cast<RackDevice&>(device).activate(&rack);
        });

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

void TrackProcessor::reportEvents(int numSamples) noexcept
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
    auto* eventTapped = eventTap.load(std::memory_order_relaxed);
    for (int i = 0; i < sink.count; ++i)
    {
        auto& e = sink.events[i];
        e.sampleOffset = std::clamp(e.sampleOffset, 0, numSamples - 1);
        if (eventTapped != nullptr)
            eventTapped->onTrackEvent(id, sink.chunkStart + e.sampleOffset, e);
        if (isNoteOn(e))
            lastNote.store(e.bytes[1], std::memory_order_relaxed);
    }
}

void TrackProcessor::renderChain(int numSamples, const DeviceTimeInfo& time, const float* const* input,
                                 int compensation) noexcept
{
    numSamples = std::min(numSamples, preparedBlockSize);
    if (numSamples <= 0)
        return;

    MidiBlock midiInput{sink.events, sink.capacity, sink.count, 0};
    sink.count = 0; // consumed: a processor without a scheduler (a return, the master) starts empty

    // A newer chain: fade the current one out first (or switch at once if nothing is audible, or if
    // the same devices carry on and only their sidechain routes changed).
    const auto* latest = chains.latest();
    if (latest != activeChain)
    {
        if (activeChain == nullptr || chainGain <= 0.0f ||
            (latest != nullptr && sameDevicesIn(activeChain->object.get(), latest->object.get())))
            switchChain(latest);
        else
            chainFade = ChainFade::Out;
    }

    for (int ch = 0; ch < 2; ++ch)
    {
        if (input != nullptr)
            std::copy_n(input[ch], numSamples, chainChannels[ch]);
        else
            std::fill_n(chainChannels[ch], numSamples, 0.0f);
    }
    const bool wantTap = tapWanted.load(std::memory_order_relaxed);
    if (activeChain != nullptr && activeChain->object != nullptr)
    {
        if (const auto lost = runner.process(*activeChain->object, chainChannels, numSamples, midiInput, time,
                                             wantTap ? tapChannels : nullptr);
            lost > 0)
            dropped.fetch_add(lost, std::memory_order_relaxed);
    }
    else if (wantTap)
    {
        for (int ch = 0; ch < 2; ++ch)
            std::copy_n(chainChannels[ch], numSamples, tapChannels[ch]);
    }

    applyChainFade(numSamples);
    if (chainFade == ChainFade::Out && chainGain <= 0.0f)
        switchChain(latest); // faded out: the new chain fades in from the next block

    // Delay compensation: line this track up with the slowest one.
    compensation = std::clamp(compensation, 0, compensationLines[0].capacity());
    for (int ch = 0; ch < 2; ++ch)
    {
        auto& line = compensationLines[ch];
        const float* in = chainChannels[ch];
        float* out = outputChannels[ch];
        for (int i = 0; i < numSamples; ++i)
        {
            line.push(in[i]);
            out[i] = line.tap(compensation);
        }
    }
}

void TrackProcessor::clearGroupInput(int numSamples) noexcept
{
    numSamples = std::min(numSamples, preparedBlockSize);
    for (int ch = 0; ch < 2; ++ch)
        std::fill_n(groupChannels[ch], std::max(0, numSamples), 0.0f);
}

void TrackProcessor::mix(int numSamples, float targetLeft, float targetRight, const Sends& sends, float* busLeft,
                         float* busRight) noexcept
{
    numSamples = std::min(numSamples, preparedBlockSize);
    if (numSamples <= 0)
        return;
    const float* inL = outputChannels[0];
    const float* inR = outputChannels[1];

    // Sends that wait (a track inside groups with latency): they take the output delayed by the
    // groups' latency, so they reach the returns when the direct path does.
    const int sendDelay = std::clamp(sends.delay, 0, sendLines[0].capacity());
    const bool delayedSends = sendDelay > 0;
    if (delayedSends)
    {
        for (int ch = 0; ch < 2; ++ch)
        {
            auto& line = sendLines[ch];
            const float* in = outputChannels[ch];
            float* out = sendChannels[ch];
            for (int i = 0; i < numSamples; ++i)
            {
                line.push(in[i]);
                out[i] = line.tap(sendDelay);
            }
        }
        inL = sendChannels[0];
        inR = sendChannels[1];
    }

    // Pre-fader sends take the (compensated) chain output as it is.
    const int numSends = std::min(sends.count, maxReturns);
    const float sendStep = 1.0f / static_cast<float>(numSamples);
    for (int r = 0; r < numSends; ++r)
    {
        if (!sends.preFader[r])
            continue;
        const float from = sendGains[static_cast<std::size_t>(r)];
        const float to = sends.audible ? sends.gains[r] : 0.0f;
        if (from == 0.0f && to == 0.0f)
            continue;
        float* outL = sends.buses[2 * r];
        float* outR = sends.buses[2 * r + 1];
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = from + (to - from) * static_cast<float>(i + 1) * sendStep;
            outL[i] += inL[i] * g;
            outR[i] += inR[i] * g;
        }
        sendGains[static_cast<std::size_t>(r)] = to;
    }

    // Fader and pan, into the bus; post-fader sends from the same signal.
    gainLeft.setTargetValue(targetLeft);
    gainRight.setTargetValue(targetRight);
    float* faded[2] = {delayed.getWritePointer(0), delayed.getWritePointer(1)}; // in place: output() is used up
    float* postSends[2] = {sendChannels[2], sendChannels[3]};
    if (gainLeft.isSmoothing() || gainRight.isSmoothing())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float gl = gainLeft.getNextValue(), gr = gainRight.getNextValue();
            faded[0][i] *= gl;
            faded[1][i] *= gr;
            if (delayedSends)
            {
                postSends[0][i] = inL[i] * gl;
                postSends[1][i] = inR[i] * gr;
            }
        }
    }
    else
    {
        juce::FloatVectorOperations::multiply(faded[0], gainLeft.getCurrentValue(), numSamples);
        juce::FloatVectorOperations::multiply(faded[1], gainRight.getCurrentValue(), numSamples);
        if (delayedSends)
        {
            juce::FloatVectorOperations::multiply(postSends[0], inL, gainLeft.getCurrentValue(), numSamples);
            juce::FloatVectorOperations::multiply(postSends[1], inR, gainRight.getCurrentValue(), numSamples);
        }
    }
    const float* postL = delayedSends ? postSends[0] : faded[0];
    const float* postR = delayedSends ? postSends[1] : faded[1];
    for (int r = 0; r < numSends; ++r)
    {
        if (sends.preFader[r])
            continue;
        const float from = sendGains[static_cast<std::size_t>(r)];
        const float to = sends.audible ? sends.gains[r] : 0.0f;
        if (from == 0.0f && to == 0.0f)
            continue;
        float* outL = sends.buses[2 * r];
        float* outR = sends.buses[2 * r + 1];
        for (int i = 0; i < numSamples; ++i)
        {
            const float g = from + (to - from) * static_cast<float>(i + 1) * sendStep;
            outL[i] += postL[i] * g;
            outR[i] += postR[i] * g;
        }
        sendGains[static_cast<std::size_t>(r)] = to;
    }

    const auto rangeLeft = juce::FloatVectorOperations::findMinAndMax(faded[0], numSamples);
    const auto rangeRight = juce::FloatVectorOperations::findMinAndMax(faded[1], numSamples);
    const float peakLeft = std::max(-rangeLeft.getStart(), rangeLeft.getEnd());
    const float peakRight = std::max(-rangeRight.getStart(), rangeRight.getEnd());
    if (peakLeft > peaks[0].load(std::memory_order_relaxed))
        peaks[0].store(peakLeft, std::memory_order_relaxed);
    if (peakRight > peaks[1].load(std::memory_order_relaxed))
        peaks[1].store(peakRight, std::memory_order_relaxed);

    if (busLeft != nullptr)
        juce::FloatVectorOperations::add(busLeft, faded[0], numSamples);
    if (busRight != nullptr)
        juce::FloatVectorOperations::add(busRight, faded[1], numSamples);
}

} // namespace lattice
