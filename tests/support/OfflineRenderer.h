#pragma once

#include "core/RealtimeCheck.h"
#include "engine/AudioEngine.h"
#include "engine/SessionController.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace lattice::test
{

/** How the OfflineRenderer cuts time into blocks (docs/03-m2-engine-session.md, "Tests"). Every
    scenario must give identical results with each of them. */
enum class BlockPattern : std::uint8_t
{
    Fixed64,
    Fixed37,
    Fixed512,
    Random
};

inline constexpr std::array<BlockPattern, 4> allBlockPatterns{BlockPattern::Fixed64, BlockPattern::Fixed37,
                                                              BlockPattern::Fixed512, BlockPattern::Random};

inline std::string describe(BlockPattern p)
{
    switch (p)
    {
    case BlockPattern::Fixed64:
        return "64-sample blocks";
    case BlockPattern::Fixed37:
        return "37-sample blocks";
    case BlockPattern::Fixed512:
        return "512-sample blocks";
    case BlockPattern::Random:
        return "random block sizes";
    }
    return {};
}

/** A MIDI event an instrument received, at its absolute sample. */
struct RecordedEvent
{
    std::int64_t trackId = 0;
    std::int64_t sample = 0;
    std::uint8_t status = 0;
    std::uint8_t data1 = 0;
    std::uint8_t data2 = 0;

    bool isNoteOn() const noexcept { return (status & 0xf0) == 0x90 && data2 > 0; }
    bool isNoteOff() const noexcept
    {
        return (status & 0xf0) == 0x80 || ((status & 0xf0) == 0x90 && data2 == 0);
    }
    bool operator==(const RecordedEvent&) const = default;
};

/** Drives AudioEngine::process() directly, with no audio device, inside a ScopedAudioThread (so any
    allocation on the audio path is a counted violation). Records every event each track's instrument
    receives with its absolute sample position, and optionally the audio output. */
class OfflineRenderer final : public TrackEventTap
{
public:
    static constexpr double sampleRate = 48000.0;
    static constexpr int maxBlock = 512;

    OfflineRenderer(AudioEngine& e, BlockPattern p, std::uint32_t seed = 12345)
        : engine(e), pattern(p), rng(seed)
    {
        engine.prepare(sampleRate, maxBlock);
        events.reserve(1 << 20);
        left.resize(maxBlock);
        right.resize(maxBlock);
    }

    ~OfflineRenderer() override
    {
        for (auto& p : tapped)
            p->setEventTap(nullptr);
    }

    /** Taps every track processor currently in the snapshot (call after adding tracks). */
    void attach(SessionController& session)
    {
        session.flush();
        for (int t = 0; t < session.set().numTracks(); ++t)
        {
            auto p = session.processorForTrack(t);
            if (p != nullptr && std::find(tapped.begin(), tapped.end(), p) == tapped.end())
            {
                p->setEventTap(this);
                tapped.push_back(p);
            }
        }
    }

    /** Renders until the engine's sample counter reaches `target`. */
    void renderTo(std::int64_t target)
    {
        while (position < target)
        {
            const int n = static_cast<int>(std::min<std::int64_t>(nextBlockSize(), target - position));
            std::array<float*, 2> outputs{left.data(), right.data()};
            {
                const rt::ScopedAudioThread audioThread;
                engine.process(outputs.data(), 2, n);
            }
            if (captureAudio)
            {
                audioLeft.insert(audioLeft.end(), left.begin(), left.begin() + n);
                audioRight.insert(audioRight.end(), right.begin(), right.begin() + n);
            }
            position += n;
        }
    }

    void renderFor(std::int64_t samples) { renderTo(position + samples); }
    void renderToBeat(double beat, double bpm = 120.0) { renderTo(samplesForBeats(beat, bpm)); }
    std::int64_t now() const noexcept { return position; }

    static std::int64_t samplesForBeats(double beats, double bpm = 120.0)
    {
        return static_cast<std::int64_t>(std::llround(beats * 60.0 * sampleRate / bpm));
    }

    void onTrackEvent(std::int64_t trackId, std::int64_t absoluteSample, const TimedMidi& e) noexcept override
    {
        if (events.size() < events.capacity())
            events.push_back({trackId, absoluteSample, e.bytes[0], e.bytes[1], e.bytes[2]});
        else
            ++overflow;
    }

    std::vector<RecordedEvent> noteOns(std::int64_t trackId) const
    {
        std::vector<RecordedEvent> result;
        for (const auto& e : events)
            if (e.trackId == trackId && e.isNoteOn())
                result.push_back(e);
        return result;
    }

    /** Pitches whose note-ons outnumber their note-offs on a track (stuck notes). */
    std::vector<int> unbalancedNotes(std::int64_t trackId) const
    {
        std::array<int, 128> balance{};
        for (const auto& e : events)
        {
            if (e.trackId != trackId)
                continue;
            if (e.isNoteOn())
                ++balance[e.data1];
            else if (e.isNoteOff() && balance[e.data1] > 0)
                --balance[e.data1];
        }
        std::vector<int> stuck;
        for (int p = 0; p < 128; ++p)
            if (balance[static_cast<std::size_t>(p)] != 0)
                stuck.push_back(p);
        return stuck;
    }

    std::vector<RecordedEvent> events;
    std::size_t overflow = 0;
    bool captureAudio = false;
    std::vector<float> audioLeft;
    std::vector<float> audioRight;

private:
    int nextBlockSize()
    {
        switch (pattern)
        {
        case BlockPattern::Fixed64:
            return 64;
        case BlockPattern::Fixed37:
            return 37;
        case BlockPattern::Fixed512:
            return 512;
        case BlockPattern::Random:
            rng = rng * 1664525u + 1013904223u;
            return 1 + static_cast<int>((rng >> 8) % maxBlock);
        }
        return 64;
    }

    AudioEngine& engine;
    BlockPattern pattern;
    std::uint32_t rng;
    std::int64_t position = 0;
    std::vector<float> left;
    std::vector<float> right;
    std::vector<std::shared_ptr<TrackProcessor>> tapped;
};

/** Adds a clip with notes. Returns the clip's tree. */
inline juce::ValueTree addClip(LiveSet& set, int track, int scene, double lengthBeats,
                               std::initializer_list<NoteData> notes)
{
    auto clip = set.createClip(track, scene, lengthBeats, "test");
    for (const auto& n : notes)
        set.addNote(clip, n);
    return clip;
}

} // namespace lattice::test
