#include "engine/session/SessionProcessor.h"

#include <algorithm>
#include <cmath>

namespace lattice
{
namespace
{
constexpr std::uint8_t statusMask = 0xf0;
constexpr std::uint8_t noteOn = 0x90;
constexpr std::uint8_t noteOff = 0x80;

const ClipData* findClip(const TrackSnapshot& track, std::int64_t clipId, int sceneHint) noexcept
{
    if (sceneHint >= 0 && sceneHint < static_cast<int>(track.slots.size()))
        if (const auto* c = track.slots[static_cast<std::size_t>(sceneHint)].clip.get();
            c != nullptr && c->id == clipId)
            return c;
    const int scene = track.sceneOfClip(clipId);
    return scene >= 0 ? track.slots[static_cast<std::size_t>(scene)].clip.get() : nullptr;
}
} // namespace

SessionProcessor::SessionProcessor(SpscFifo<EngineEvent>& eventsToMessageThread)
    : eventsOut(eventsToMessageThread)
{
}

void SessionProcessor::prepare(double newSampleRate, int maxBlockSize)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    sampleRate = newSampleRate;
    maxBlock = std::max(maxBlockSize, 1);
    clock.prepare(newSampleRate);
    metronome.prepare(newSampleRate);
    now = 0;
    lastGeneration = 0;
    lastSnapshotTempo = std::numeric_limits<double>::quiet_NaN();
    lastSnapshotSigNum = 0;
    lastSnapshotSigDen = 0;
    pendingScene = {};
    sessionRecord = false;
    stopAllBeat = never;
    metronomeBuffer.assign(static_cast<std::size_t>(maxBlock), 0.0f);
    returnInputs.setSize(2 * TrackProcessor::maxReturns, maxBlock, false, true, false);
    for (int i = 0; i < 2 * TrackProcessor::maxReturns; ++i)
        returnInputPointers[static_cast<std::size_t>(i)] = returnInputs.getWritePointer(i);
    trackBus.setSize(2, maxBlock, false, true, false);
    returnBus.setSize(2, maxBlock, false, true, false);
    masterInput.setSize(2, maxBlock, false, true, false);
    for (auto& line : masterInputDelay)
        line.prepare(static_cast<int>(TrackProcessor::maxCompensationSeconds * sampleRate) + 4);
    for (auto& g : returnGains)
    {
        g.reset(sampleRate, TrackProcessor::gainRampSeconds);
        g.setCurrentAndTargetValue(0.0f);
    }
    masterLeft.reset(newSampleRate, TrackProcessor::gainRampSeconds);
    masterRight.reset(newSampleRate, TrackProcessor::gainRampSeconds);
    masterLeft.setCurrentAndTargetValue(0.0f);
    masterRight.setCurrentAndTargetValue(0.0f);
    published.store({});
}

std::array<float, 2> SessionProcessor::takeReturnPeaks(int r) noexcept
{
    if (r < 0 || r >= TrackProcessor::maxReturns)
        return {0.0f, 0.0f};
    auto& p = returnPeaks[static_cast<std::size_t>(r)];
    return {p[0].exchange(0.0f, std::memory_order_relaxed), p[1].exchange(0.0f, std::memory_order_relaxed)};
}

const float* const* SessionProcessor::channels(const SidechainRoute& route) const noexcept
{
    if (rendering == nullptr)
        return nullptr;
    for (const auto& t : rendering->tracks)
        if (t.id == route.trackId)
            return route.preFx ? t.processor->preFxTap() : t.processor->chainOutput();
    return nullptr;
}

namespace
{
/** One parallel job's shared, read-only context. */
struct ChainJob
{
    const EngineSnapshot* snapshot = nullptr;
    const std::vector<int>* level = nullptr;
    const DeviceTimeInfo* time = nullptr;
    float* const* returnInputs = nullptr;
    int numSamples = 0;
};

void renderTrackTask(void* context, int index) noexcept
{
    const auto& job = *static_cast<const ChainJob*>(context);
    const auto& track = job.snapshot->tracks[static_cast<std::size_t>((*job.level)[static_cast<std::size_t>(index)])];
    track.processor->renderChain(job.numSamples, *job.time, nullptr, track.compensation);
}

void renderReturnTask(void* context, int index) noexcept
{
    const auto& job = *static_cast<const ChainJob*>(context);
    const auto& ret = job.snapshot->returns[static_cast<std::size_t>(index)];
    const float* input[2] = {job.returnInputs[2 * index], job.returnInputs[2 * index + 1]};
    ret.processor->renderChain(job.numSamples, *job.time, input, ret.compensation);
}

void runOn(const WorkerPool* pool, int count, WorkerPool::Task task, ChainJob& job) noexcept
{
    if (pool != nullptr)
    {
        pool->run(count, task, &job);
        return;
    }
    for (int i = 0; i < count; ++i)
        task(&job, i);
}
} // namespace

void SessionProcessor::renderMix(const EngineSnapshot& snapshot, const DeviceTimeInfo& time, int n, float* left,
                                 float* right, const WorkerPool* pool) noexcept
{
    rendering = &snapshot;
    DeviceTimeInfo t = time;
    t.sidechains = this;
    ChainJob job;
    job.snapshot = &snapshot;
    job.time = &t;
    job.numSamples = n;
    job.returnInputs = returnInputPointers.data();

    for (const auto& track : snapshot.tracks)
        track.processor->reportEvents(n);

    // 1. Every track's chain, level by level (sidechain sources before their listeners); the
    //    tracks of a level in parallel. Each writes only its own buffers.
    for (const auto& level : snapshot.levels)
    {
        job.level = &level;
        runOn(pool, static_cast<int>(level.size()), renderTrackTask, job);
    }

    // 2. Faders, pans and sends, in track order: the same sums whatever ran in parallel.
    const int numReturns = static_cast<int>(snapshot.returns.size());
    for (int r = 0; r < 2 * numReturns; ++r)
        std::fill_n(returnInputPointers[static_cast<std::size_t>(r)], n, 0.0f);
    float* busL = trackBus.getWritePointer(0);
    float* busR = trackBus.getWritePointer(1);
    std::fill_n(busL, n, 0.0f);
    std::fill_n(busR, n, 0.0f);
    std::array<bool, TrackProcessor::maxReturns> preFader{};
    for (int r = 0; r < numReturns; ++r)
        preFader[static_cast<std::size_t>(r)] = snapshot.returns[static_cast<std::size_t>(r)].preFader;
    for (const auto& track : snapshot.tracks)
    {
        // Soloing tracks keeps the returns audible ("solo in place"): a soloed track still reaches
        // them, the others don't. Soloing a return silences the tracks' direct outputs.
        const bool trackAudible = !track.mute && (!snapshot.anySolo || track.solo);
        const bool direct = trackAudible && !snapshot.anyReturnSolo;
        TrackProcessor::Sends sends;
        sends.gains = track.sendGains.data();
        sends.preFader = preFader.data();
        sends.buses = returnInputPointers.data();
        sends.count = numReturns;
        sends.audible = trackAudible;
        track.processor->mix(n, direct ? track.gainLeft : 0.0f, direct ? track.gainRight : 0.0f, sends, busL, busR);
    }

    // 3. The returns, then their faders.
    float* retL = returnBus.getWritePointer(0);
    float* retR = returnBus.getWritePointer(1);
    std::fill_n(retL, n, 0.0f);
    std::fill_n(retR, n, 0.0f);
    runOn(pool, numReturns, renderReturnTask, job); // the returns' chains, in parallel
    for (int r = 0; r < numReturns; ++r)
    {
        const auto& ret = snapshot.returns[static_cast<std::size_t>(r)];
        const bool audible = !ret.mute && (!snapshot.anyReturnSolo || ret.solo);
        TrackProcessor::Sends none;
        ret.processor->mix(n, audible ? ret.gainLeft : 0.0f, audible ? ret.gainRight : 0.0f, none, retL, retR);
    }

    // 4. The master: the tracks (delayed to line up with the returns) and the returns, through the
    //    master's chain.
    float* inL = masterInput.getWritePointer(0);
    float* inR = masterInput.getWritePointer(1);
    const int wait = std::clamp(snapshot.masterInputDelay, 0, masterInputDelay[0].capacity());
    for (int i = 0; i < n; ++i)
    {
        masterInputDelay[0].push(busL[i]);
        masterInputDelay[1].push(busR[i]);
        inL[i] = masterInputDelay[0].tap(wait) + retL[i];
        inR[i] = masterInputDelay[1].tap(wait) + retR[i];
    }
    if (snapshot.master != nullptr)
    {
        const float* input[2] = {inL, inR};
        snapshot.master->renderChain(n, t, input, 0);
        const auto* out = snapshot.master->chainOutput();
        juce::FloatVectorOperations::add(left, out[0], n);
        juce::FloatVectorOperations::add(right, out[1], n);
    }
    else
    {
        juce::FloatVectorOperations::add(left, inL, n);
        juce::FloatVectorOperations::add(right, inR, n);
    }
    rendering = nullptr;
}

std::array<float, 2> SessionProcessor::takeMasterPeaks() noexcept
{
    return {masterPeaks[0].exchange(0.0f, std::memory_order_relaxed),
            masterPeaks[1].exchange(0.0f, std::memory_order_relaxed)};
}

double SessionProcessor::nextRandom() noexcept
{
    // xorshift64*: deterministic for a given seed, so follow-action tests can check outcomes.
    randomState ^= randomState >> 12;
    randomState ^= randomState << 25;
    randomState ^= randomState >> 27;
    const auto r = randomState * 0x2545f4914f6cdd1dull;
    return static_cast<double>(r >> 11) * 0x1.0p-53;
}

// --- Chunk processing -----------------------------------------------------------------------------

void SessionProcessor::process(const EngineSnapshot* snapshot, const EngineCommand* commands, int numCommands,
                               const TimedMidi* liveInput, int numLiveInput, int numSamples, float* left,
                               float* right, const WorkerPool* pool) noexcept
{
    numSamples = std::min(numSamples, maxBlock);
    std::fill(left, left + numSamples, 0.0f);
    std::fill(right, right + numSamples, 0.0f);
    std::fill(metronomeBuffer.begin(), metronomeBuffer.begin() + numSamples, 0.0f);

    const std::int64_t s0 = now;
    const std::int64_t s1 = now + numSamples;
    if (snapshot == nullptr)
    {
        now = s1;
        return;
    }

    for (const auto& t : snapshot->tracks)
        t.processor->beginChunk(s0);

    if (snapshot->generation != lastGeneration)
        syncWithSnapshot(*snapshot, s0);
    globalQuantization = snapshot->globalQuantization;

    for (int i = 0; i < numCommands; ++i)
        applyCommand(*snapshot, commands[i], s0);

    fireDue(*snapshot, s0);
    for (const auto& t : snapshot->tracks)
        updateCapture(t, s0);
    routeLiveInput(*snapshot, liveInput, numLiveInput, s0);

    // Devices see the transport as it is at the chunk's start (after commands applied there).
    DeviceTimeInfo time;
    time.sampleRate = sampleRate;
    time.tempo = clock.tempo();
    time.samplesPerBeat = clock.samplesPerBeat();
    time.playing = clock.isPlaying();
    time.beatAtStart = time.playing ? clock.beatAt(s0) : freeRunningBeat;
    time.blockStartSample = s0;
    freeRunningBeat = time.beatAtStart + numSamples / time.samplesPerBeat;

    // Walk the chunk from one scheduled action to the next.
    std::int64_t cur = s0;
    for (int guard = 0; guard < 4096; ++guard)
    {
        const auto boundary = nextScheduledSample(*snapshot, cur, s1);
        for (const auto& t : snapshot->tracks)
        {
            auto& st = t.processor->session();
            if (st.player.isActive() &&
                st.player.render(clock, cur, boundary, t.processor->events()) == ClipPlayer::Status::Ended)
            {
                st.followBeat = never;
                updateCapture(t, boundary);
            }
        }
        if (snapshot->metronomeOn)
            metronome.render(clock, cur, boundary, s0, metronomeBuffer.data(), snapshot->metronomeGain);
        else
            metronome.silence();

        if (boundary >= s1)
            break;
        fireDue(*snapshot, boundary);
        cur = boundary;
    }

    // Instruments, effects, mixer, returns, master chain.
    renderMix(*snapshot, time, numSamples, left, right, pool);

    masterLeft.setTargetValue(snapshot->masterGainLeft);
    masterRight.setTargetValue(snapshot->masterGainRight);
    if (masterLeft.isSmoothing() || masterRight.isSmoothing())
    {
        for (int i = 0; i < numSamples; ++i)
        {
            left[i] *= masterLeft.getNextValue();
            right[i] *= masterRight.getNextValue();
        }
    }
    else
    {
        juce::FloatVectorOperations::multiply(left, masterLeft.getCurrentValue(), numSamples);
        juce::FloatVectorOperations::multiply(right, masterRight.getCurrentValue(), numSamples);
    }

    const auto rangeLeft = juce::FloatVectorOperations::findMinAndMax(left, numSamples);
    const auto rangeRight = juce::FloatVectorOperations::findMinAndMax(right, numSamples);
    const float peakLeft = std::max(-rangeLeft.getStart(), rangeLeft.getEnd());
    const float peakRight = std::max(-rangeRight.getStart(), rangeRight.getEnd());
    if (peakLeft > masterPeaks[0].load(std::memory_order_relaxed))
        masterPeaks[0].store(peakLeft, std::memory_order_relaxed);
    if (peakRight > masterPeaks[1].load(std::memory_order_relaxed))
        masterPeaks[1].store(peakRight, std::memory_order_relaxed);

    // The click is not affected by the master fader.
    juce::FloatVectorOperations::add(left, metronomeBuffer.data(), numSamples);
    juce::FloatVectorOperations::add(right, metronomeBuffer.data(), numSamples);

    now = s1;
    publish(*snapshot);
}

void SessionProcessor::syncWithSnapshot(const EngineSnapshot& snapshot, std::int64_t sample) noexcept
{
    lastGeneration = snapshot.generation;

    // Tempo and time signature follow the document only when the document's value changes, so a
    // scene's tempo isn't undone by a snapshot built before the model caught up with it.
    if (snapshot.tempo != lastSnapshotTempo)
    {
        clock.setTempo(snapshot.tempo, sample);
        lastSnapshotTempo = snapshot.tempo;
    }
    if (snapshot.timeSigNum != lastSnapshotSigNum || snapshot.timeSigDen != lastSnapshotSigDen)
    {
        clock.setTimeSignature(snapshot.timeSigNum, snapshot.timeSigDen, clock.beatAt(sample));
        lastSnapshotSigNum = snapshot.timeSigNum;
        lastSnapshotSigDen = snapshot.timeSigDen;
    }

    for (const auto& t : snapshot.tracks)
    {
        auto& st = t.processor->session();
        if (!st.player.isActive())
            continue;

        const int scene = t.sceneOfClip(st.player.clipId());
        if (scene < 0)
        {
            stopTrack(t, sample); // the playing clip was deleted or moved to another track
            continue;
        }
        const ClipData* clip = t.slots[static_cast<std::size_t>(scene)].clip.get();
        if (clip == st.player.clip())
            continue;

        if (st.player.replaceClipData(clip, clock, sample, t.processor->events()) ==
            ClipPlayer::Status::Ended)
        {
            st.followBeat = never;
            updateCapture(t, sample);
            continue;
        }
        if (st.followBeat != never || clip->hasFollowActions())
        {
            const double due = st.player.launchBeat() + clip->followTime;
            st.followBeat = clip->hasFollowActions() && due >= clock.beatAt(sample) ? due : never;
        }
    }
}

void SessionProcessor::ensurePlaying(std::int64_t sample) noexcept
{
    if (!clock.isPlaying())
        clock.start(sample, 0.0); // launching while stopped starts the transport at bar 1
}

double SessionProcessor::quantizedBeat(Quantization q, double beat) const noexcept
{
    if (q == Quantization::Global)
        q = globalQuantization;
    const double grid = quantizationBeats(q, clock.beatsPerBar());
    return grid > 0.0 ? clock.nextGridLine(beat, grid) : beat;
}

double SessionProcessor::clipQuantizedBeat(const ClipData& clip, double beat) const noexcept
{
    return quantizedBeat(clip.quantization, beat);
}

void SessionProcessor::setPending(const TrackSnapshot& track,
                                  const TrackSessionState::Pending& pending) noexcept
{
    auto& st = track.processor->session();
    if (st.pending.kind == Queued::Record &&
        !(pending.kind == Queued::Record && pending.clipId == st.pending.clipId))
    {
        EngineEvent e;
        e.type = EngineEventType::RecordingCancelled;
        e.trackId = track.id;
        e.clipId = st.pending.clipId;
        post(e);
    }
    st.pending = pending;
}

void SessionProcessor::queueStop(const TrackSnapshot& track, double beat) noexcept
{
    TrackSessionState::Pending p;
    p.kind = Queued::Stop;
    p.beat = beat;
    setPending(track, p);
    track.processor->session().stopAfterLaunch = false;
}

void SessionProcessor::stopAllNow(const EngineSnapshot& snapshot, std::int64_t sample) noexcept
{
    for (const auto& t : snapshot.tracks)
    {
        setPending(t, {});
        stopTrack(t, sample);
    }
    stopAllBeat = never;
}

void SessionProcessor::applyCommand(const EngineSnapshot& snapshot, const EngineCommand& command,
                                    std::int64_t sample) noexcept
{
    const auto* track = snapshot.findTrack(command.trackId);
    const auto slotOf = [&](const TrackSnapshot& t) -> const SlotSnapshot* {
        if (command.scene < 0 || command.scene >= static_cast<int>(t.slots.size()))
            return nullptr;
        return &t.slots[static_cast<std::size_t>(command.scene)];
    };

    switch (command.type)
    {
    case CommandType::LaunchSlot:
    {
        if (track == nullptr)
            return;
        const auto* slot = slotOf(*track);
        if (slot == nullptr)
            return;
        auto& st = track->processor->session();
        if (slot->clip == nullptr || slot->clip->id != command.clipId)
        {
            // An empty slot's stop button (a stale clip ID is ignored).
            if (command.clipId == 0 && slot->hasStopButton && clock.isPlaying() &&
                (st.player.isActive() || st.pending.kind != Queued::None))
                queueStop(*track, quantizedBeat(Quantization::Global, clock.beatAt(sample)));
            return;
        }

        const auto& clip = *slot->clip;
        if (clip.launchMode == LaunchMode::Toggle && clock.isPlaying())
        {
            if (st.pending.kind == Queued::Launch && st.pending.clipId == clip.id)
            {
                st.pending = {};
                return;
            }
            if (st.player.clipId() == clip.id)
            {
                queueStop(*track, clipQuantizedBeat(clip, clock.beatAt(sample)));
                return;
            }
        }

        const bool wasStopped = !clock.isPlaying();
        ensurePlaying(sample);
        TrackSessionState::Pending p;
        p.kind = Queued::Launch;
        p.clipId = clip.id;
        p.scene = command.scene;
        p.beat = wasStopped ? 0.0 : clipQuantizedBeat(clip, clock.beatAt(sample));
        setPending(*track, p);
        st.stopAfterLaunch = false;
        return;
    }

    case CommandType::ReleaseSlot:
    {
        if (track == nullptr)
            return;
        const auto* slot = slotOf(*track);
        if (slot == nullptr || slot->clip == nullptr || slot->clip->id != command.clipId ||
            slot->clip->launchMode != LaunchMode::Gate)
            return;
        auto& st = track->processor->session();
        if (st.pending.kind == Queued::Launch && st.pending.clipId == command.clipId)
            st.stopAfterLaunch = true;
        else if (st.player.clipId() == command.clipId && clock.isPlaying())
            queueStop(*track, clipQuantizedBeat(*slot->clip, clock.beatAt(sample)));
        return;
    }

    case CommandType::LaunchScene:
    {
        if (command.scene < 0)
            return;
        const auto scene = static_cast<std::size_t>(command.scene);
        const bool anyClip =
            std::any_of(snapshot.tracks.begin(), snapshot.tracks.end(), [&](const TrackSnapshot& t) {
                return scene < t.slots.size() && t.slots[scene].clip != nullptr;
            });
        if (!anyClip && !clock.isPlaying())
            return;

        const bool wasStopped = !clock.isPlaying();
        ensurePlaying(sample);
        const double beat = clock.beatAt(sample);
        const double sceneBeat = wasStopped ? 0.0 : quantizedBeat(Quantization::Global, beat);

        if (scene < snapshot.scenes.size())
        {
            const auto& sc = snapshot.scenes[scene];
            if (sc.tempo > 0.0 || sc.timeSigNum > 0)
                pendingScene = {sceneBeat, sc.tempo, sc.timeSigNum, sc.timeSigDen};
        }

        for (const auto& t : snapshot.tracks)
        {
            if (scene >= t.slots.size())
                continue;
            const auto& slot = t.slots[scene];
            auto& st = t.processor->session();
            if (slot.clip != nullptr)
            {
                TrackSessionState::Pending p;
                p.kind = Queued::Launch;
                p.clipId = slot.clip->id;
                p.scene = command.scene;
                p.beat = wasStopped ? 0.0 : clipQuantizedBeat(*slot.clip, beat);
                setPending(t, p);
                st.stopAfterLaunch = false;
            }
            else if (slot.hasStopButton && (st.player.isActive() || st.pending.kind != Queued::None))
            {
                queueStop(t, sceneBeat);
            }
        }
        return;
    }

    case CommandType::StopTrack:
    {
        if (track == nullptr || !clock.isPlaying())
            return;
        auto& st = track->processor->session();
        if (st.player.isActive() || st.pending.kind != Queued::None)
            queueStop(*track, quantizedBeat(Quantization::Global, clock.beatAt(sample)));
        return;
    }

    case CommandType::StopAll:
    {
        if (!clock.isPlaying())
            return;
        if (stopAllBeat != never)
        {
            stopAllNow(snapshot, sample); // second press: stop at once
            return;
        }
        stopAllBeat = quantizedBeat(Quantization::Global, clock.beatAt(sample));
        for (const auto& t : snapshot.tracks)
        {
            const auto& st = t.processor->session();
            if (st.player.isActive() || st.pending.kind != Queued::None)
                queueStop(t, stopAllBeat);
        }
        return;
    }

    case CommandType::Play:
        if (!clock.isPlaying())
            clock.start(sample, 0.0);
        return;

    case CommandType::Continue:
        if (!clock.isPlaying())
            clock.start(sample, clock.stoppedBeat());
        return;

    case CommandType::Stop:
        if (clock.isPlaying())
        {
            stopAllNow(snapshot, sample);
            clock.stop(sample);
        }
        else
        {
            clock.rewind();
        }
        pendingScene = {};
        stopAllBeat = never;
        return;

    case CommandType::StartRecording:
    {
        if (track == nullptr)
            return;
        const auto* slot = slotOf(*track);
        if (slot == nullptr || slot->clip == nullptr || slot->clip->id != command.clipId)
            return;
        const bool wasStopped = !clock.isPlaying();
        ensurePlaying(sample);
        TrackSessionState::Pending p;
        p.kind = Queued::Record;
        p.clipId = command.clipId;
        p.scene = command.scene;
        p.beat = wasStopped ? 0.0 : quantizedBeat(Quantization::Global, clock.beatAt(sample));
        p.openEnded = command.flag;
        setPending(*track, p);
        track->processor->session().stopAfterLaunch = false;
        return;
    }

    case CommandType::EndRecording:
    {
        if (track == nullptr)
            return;
        auto& st = track->processor->session();
        if (st.recordingNewClip && st.recordingOpenEnded)
            st.endRecordingBeat = quantizedBeat(Quantization::Global, clock.beatAt(sample));
        else if (st.pending.kind == Queued::Record)
            setPending(*track, {});
        return;
    }

    case CommandType::SetSessionRecord:
        sessionRecord = command.flag;
        if (!sessionRecord)
        {
            // Turning session record off ends open-length recordings now (rounded up to the bar).
            for (const auto& t : snapshot.tracks)
            {
                auto& st = t.processor->session();
                if (st.recordingNewClip && st.recordingOpenEnded)
                    st.endRecordingBeat = clock.beatAt(sample);
            }
        }
        return;

    case CommandType::SetRandomSeed:
        randomState = command.value != 0 ? command.value : 0x9e3779b97f4a7c15ull;
        return;
    }
}

void SessionProcessor::routeLiveInput(const EngineSnapshot& snapshot, const TimedMidi* input, int numInput,
                                      std::int64_t sample) noexcept
{
    if (numInput <= 0)
        return;
    const double beat = clock.beatAt(sample);

    for (const auto& t : snapshot.tracks)
    {
        if (!t.arm)
            continue;
        auto& sink = t.processor->events();
        auto& st = t.processor->session();
        for (int i = 0; i < numInput; ++i)
        {
            const auto& e = input[i];
            sink.add(sample, e);
            if (st.captureClipId == 0 || e.size < 3)
                continue;

            const auto status = static_cast<std::uint8_t>(e.bytes[0] & statusMask);
            const auto pitch = static_cast<std::uint8_t>(e.bytes[1] & 0x7f);
            const auto velocity = static_cast<std::uint8_t>(e.bytes[2] & 0x7f);
            EngineEvent out;
            out.trackId = t.id;
            out.clipId = st.captureClipId;
            out.pitch = pitch;
            out.beat = beat;
            if (status == noteOn && velocity > 0)
            {
                if (st.captureHeld[pitch])
                {
                    out.type = EngineEventType::CaptureNoteOff;
                    post(out);
                }
                out.type = EngineEventType::CaptureNoteOn;
                out.velocity = velocity;
                out.position = st.player.positionAt(beat);
                post(out);
                st.captureHeld[pitch] = true;
            }
            else if ((status == noteOff || status == noteOn) && st.captureHeld[pitch])
            {
                out.type = EngineEventType::CaptureNoteOff;
                post(out);
                st.captureHeld[pitch] = false;
            }
        }
    }
}

// --- Scheduling -------------------------------------------------------------------------------------

bool SessionProcessor::isDue(double beat, std::int64_t sample) const noexcept
{
    return beat != never && clock.isPlaying() && clock.sampleAtBeat(beat) <= sample;
}

std::int64_t SessionProcessor::nextScheduledSample(const EngineSnapshot& snapshot, std::int64_t from,
                                                   std::int64_t limit) const noexcept
{
    if (!clock.isPlaying())
        return limit;

    std::int64_t best = limit;
    const auto consider = [&](double beat) {
        if (beat != never)
            best = std::min(best, std::max(from, clock.sampleAtBeat(beat)));
    };
    consider(pendingScene.beat);
    for (const auto& t : snapshot.tracks)
    {
        const auto& st = t.processor->session();
        if (st.pending.kind != Queued::None)
            consider(st.pending.beat);
        consider(st.followBeat);
        consider(st.endRecordingBeat);
        if (st.recordingNewClip)
            consider(st.recordingEndBeat);
    }
    return best;
}

void SessionProcessor::fireDue(const EngineSnapshot& snapshot, std::int64_t sample) noexcept
{
    // An action can schedule another for the same moment (a follow action launching a clip), so
    // repeat until nothing more is due.
    for (int round = 0; round < 64; ++round)
    {
        bool fired = false;

        if (isDue(pendingScene.beat, sample))
        {
            const auto change = pendingScene;
            pendingScene = {};
            if (change.tempo > 0.0)
            {
                clock.setTempo(change.tempo, sample);
                EngineEvent e;
                e.type = EngineEventType::TempoChanged;
                e.value = clock.tempo();
                post(e);
            }
            if (change.timeSigNum > 0)
            {
                clock.setTimeSignature(change.timeSigNum, change.timeSigDen, change.beat);
                EngineEvent e;
                e.type = EngineEventType::TimeSignatureChanged;
                e.numerator = static_cast<std::int16_t>(change.timeSigNum);
                e.denominator = static_cast<std::int16_t>(change.timeSigDen);
                post(e);
            }
            fired = true;
        }

        for (const auto& t : snapshot.tracks)
        {
            auto& st = t.processor->session();
            if (st.recordingNewClip && isDue(st.recordingEndBeat, sample))
            {
                // A fixed-length recording is complete: the clip carries on as playback.
                st.recordingNewClip = false;
                st.recordingEndBeat = never;
                endCapture(t, sample, st.player.effectiveLoopEnd());
                updateCapture(t, sample);
                fired = true;
            }
            if (isDue(st.endRecordingBeat, sample))
            {
                st.endRecordingBeat = never;
                finishRecording(t, sample);
                updateCapture(t, sample);
                fired = true;
            }
            if (st.pending.kind != Queued::None && isDue(st.pending.beat, sample))
            {
                executePending(snapshot, t, sample);
                fired = true;
            }
            if (isDue(st.followBeat, sample))
            {
                fireFollowAction(snapshot, t, sample);
                fired = true;
            }
        }

        if (stopAllBeat != never && clock.sampleAtBeat(stopAllBeat) <= sample)
            stopAllBeat = never;
        if (!fired)
            break;
    }
}

void SessionProcessor::executePending(const EngineSnapshot& snapshot, const TrackSnapshot& track,
                                      std::int64_t sample) noexcept
{
    juce::ignoreUnused(snapshot);
    auto& st = track.processor->session();
    const auto p = st.pending;
    st.pending = {};

    switch (p.kind)
    {
    case Queued::Launch:
    {
        const ClipData* clip = findClip(track, p.clipId, p.scene);
        if (clip == nullptr)
        {
            st.stopAfterLaunch = false;
            return;
        }
        launchClip(track, *clip, p.beat, sample);
        if (st.stopAfterLaunch)
        {
            // Gate: released before the launch happened. Play until the next grid line.
            st.stopAfterLaunch = false;
            auto q = clip->quantization == Quantization::Global ? globalQuantization : clip->quantization;
            const double grid = quantizationBeats(q, clock.beatsPerBar());
            if (grid > 0.0)
                queueStop(track, clock.nextGridLine(p.beat + grid * 0.5, grid));
            else
                stopTrack(track, sample);
        }
        return;
    }

    case Queued::Stop:
        stopTrack(track, sample);
        return;

    case Queued::Record:
    {
        const ClipData* clip = findClip(track, p.clipId, p.scene);
        if (clip == nullptr)
        {
            EngineEvent e;
            e.type = EngineEventType::RecordingCancelled;
            e.trackId = track.id;
            e.clipId = p.clipId;
            post(e);
            return;
        }
        if (st.recordingNewClip)
            finishRecording(track, sample);
        st.player.stop(sample, track.processor->events());
        st.followBeat = never;
        st.player.start(clip, p.beat, 0.0, p.openEnded);
        st.recordingNewClip = true;
        st.recordingOpenEnded = p.openEnded;
        st.recordingStartBeat = p.beat;
        st.recordingEndBeat = p.openEnded ? never : p.beat + clip->loopEnd;
        st.endRecordingBeat = never;
        updateCapture(track, sample);
        return;
    }

    case Queued::None:
        return;
    }
}

void SessionProcessor::launchClip(const TrackSnapshot& track, const ClipData& clip, double beat,
                                  std::int64_t sample) noexcept
{
    auto& st = track.processor->session();
    double startPosition = clip.startMarker;
    if (clip.legato && st.player.isActive())
    {
        // Legato: carry on from the old clip's position, wrapped into this clip's loop.
        const double length = clip.loopLength();
        const double offset = std::fmod(st.player.positionAt(beat) - clip.loopStart, length);
        startPosition = clip.loopStart + (offset < 0.0 ? offset + length : offset);
    }

    if (st.recordingNewClip)
        finishRecording(track, sample);
    st.player.stop(sample, track.processor->events());
    st.player.start(&clip, beat, startPosition);
    st.followBeat = clip.hasFollowActions() ? beat + clip.followTime : never;
    updateCapture(track, sample);
}

void SessionProcessor::stopTrack(const TrackSnapshot& track, std::int64_t sample) noexcept
{
    auto& st = track.processor->session();
    if (st.recordingNewClip)
        finishRecording(track, sample);
    st.player.stop(sample, track.processor->events());
    st.followBeat = never;
    st.stopAfterLaunch = false;
    st.endRecordingBeat = never;
    updateCapture(track, sample);
}

void SessionProcessor::fireFollowAction(const EngineSnapshot& snapshot, const TrackSnapshot& track,
                                        std::int64_t sample) noexcept
{
    juce::ignoreUnused(snapshot);
    auto& st = track.processor->session();
    const double beat = st.followBeat;
    st.followBeat = never;
    const ClipData* clip = st.player.clip();
    if (clip == nullptr)
        return;

    const FollowAction action = nextRandom() < clip->followChanceA ? clip->followA : clip->followB;
    const int current = track.sceneOfClip(clip->id);
    const int count = static_cast<int>(track.slots.size());
    const auto hasClip = [&](int scene) {
        return track.slots[static_cast<std::size_t>(scene)].clip != nullptr;
    };
    const auto launchScene = [&](int scene) {
        if (scene >= 0 && scene < count && hasClip(scene))
            launchClip(track, *track.slots[static_cast<std::size_t>(scene)].clip, beat, sample);
    };

    switch (action)
    {
    case FollowAction::None:
        return;
    case FollowAction::Stop:
        stopTrack(track, sample);
        return;
    case FollowAction::PlayAgain:
        launchClip(track, *clip, beat, sample);
        return;
    case FollowAction::Previous:
        for (int i = 1; i <= count; ++i)
        {
            const int scene = ((current - i) % count + count) % count;
            if (hasClip(scene))
                return launchScene(scene);
        }
        return;
    case FollowAction::Next:
        for (int i = 1; i <= count; ++i)
        {
            const int scene = (current + i) % count;
            if (hasClip(scene))
                return launchScene(scene);
        }
        return;
    case FollowAction::First:
        for (int scene = 0; scene < count; ++scene)
            if (hasClip(scene))
                return launchScene(scene);
        return;
    case FollowAction::Last:
        for (int scene = count - 1; scene >= 0; --scene)
            if (hasClip(scene))
                return launchScene(scene);
        return;
    case FollowAction::Any:
    case FollowAction::Other:
    {
        const bool other = action == FollowAction::Other;
        int candidates = 0;
        for (int scene = 0; scene < count; ++scene)
            if (hasClip(scene) && !(other && scene == current))
                ++candidates;
        if (candidates == 0)
            return;
        int pick = std::min(static_cast<int>(nextRandom() * candidates), candidates - 1);
        for (int scene = 0; scene < count; ++scene)
        {
            if (!hasClip(scene) || (other && scene == current))
                continue;
            if (pick-- == 0)
                return launchScene(scene);
        }
        return;
    }
    }
}

// --- Recording ---------------------------------------------------------------------------------------

void SessionProcessor::finishRecording(const TrackSnapshot& track, std::int64_t sample) noexcept
{
    auto& st = track.processor->session();
    if (!st.recordingNewClip)
        return;
    st.recordingNewClip = false;
    st.endRecordingBeat = never;
    st.recordingEndBeat = never;

    double length = st.player.effectiveLoopEnd();
    if (st.recordingOpenEnded)
    {
        const double bar = clock.beatsPerBar();
        const double elapsed = clock.beatAt(sample) - st.recordingStartBeat;
        length = std::max(1.0, std::ceil(elapsed / bar - 1.0e-9)) * bar;
        st.player.finishOpenEnded(length);
    }
    endCapture(track, sample, length);
}

void SessionProcessor::updateCapture(const TrackSnapshot& track, std::int64_t sample) noexcept
{
    auto& st = track.processor->session();
    std::int64_t desired = 0;
    if (st.recordingNewClip)
        desired = st.player.clipId();
    else if (sessionRecord && track.arm && clock.isPlaying() && st.player.isActive())
        desired = st.player.clipId(); // overdub

    if (desired == st.captureClipId)
        return;
    if (st.captureClipId != 0)
        endCapture(track, sample, 0.0);
    st.captureClipId = desired;
    st.captureHeld = {};
}

void SessionProcessor::endCapture(const TrackSnapshot& track, std::int64_t sample,
                                  double finalLength) noexcept
{
    auto& st = track.processor->session();
    const auto clipId = st.captureClipId != 0 ? st.captureClipId : st.player.clipId();
    const double beat = clock.beatAt(sample);

    EngineEvent e;
    e.trackId = track.id;
    e.clipId = clipId;
    e.beat = beat;
    if (st.captureClipId != 0)
    {
        // Notes still held get their note-off at the end point.
        e.type = EngineEventType::CaptureNoteOff;
        for (std::size_t p = 0; p < st.captureHeld.size(); ++p)
        {
            if (st.captureHeld[p])
            {
                e.pitch = static_cast<std::uint8_t>(p);
                post(e);
            }
        }
    }
    st.captureHeld = {};
    st.captureClipId = 0;

    if (clipId != 0)
    {
        e.type = EngineEventType::CaptureEnded;
        e.pitch = 0;
        e.value = finalLength;
        post(e);
    }
}

void SessionProcessor::publish(const EngineSnapshot& snapshot) noexcept
{
    const double beat = clock.beatAt(now);
    for (const auto& t : snapshot.tracks)
    {
        const auto& st = t.processor->session();
        TrackPlayState ps;
        ps.playingClipId = st.player.clipId();
        ps.queued = st.pending.kind;
        ps.queuedClipId = st.pending.clipId;
        ps.queuedScene = st.pending.scene;
        ps.recording = st.recordingNewClip || st.captureClipId != 0;
        if (st.player.isActive())
        {
            ps.position = st.player.positionAt(beat);
            ps.loopStart = st.player.effectiveLoopStart();
            ps.loopEnd = st.player.effectiveLoopEnd();
        }
        t.processor->publishPlayState(ps);
    }

    TransportState ts;
    ts.playing = clock.isPlaying();
    ts.sessionRecord = sessionRecord;
    ts.stopAllQueued = stopAllBeat != never;
    ts.timeSigNum = static_cast<std::int16_t>(clock.timeSigNumerator());
    ts.timeSigDen = static_cast<std::int16_t>(clock.timeSigDenominator());
    ts.barOriginIndex = clock.barGridOriginIndex();
    ts.beat = beat;
    ts.tempo = clock.tempo();
    ts.barOrigin = clock.barGridOrigin();
    ts.sample = now;
    published.store(ts);
}

} // namespace lattice
