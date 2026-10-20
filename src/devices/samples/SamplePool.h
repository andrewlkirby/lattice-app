#pragma once

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace lattice
{

/** A decoded sample: 32-bit float, one or two channels, fully in memory (docs/04-m3-devices.md,
    "Sampler": memory). Each channel is padded with silence on both sides, so interpolators can read
    around any position without bounds checks. Immutable once built; shared. */
struct SampleData
{
    /** Silence on each side of every channel: enough for the longest sinc kernel. */
    static int padding();

    juce::String path;
    juce::String name;
    double sampleRate = 48000.0;
    int numChannels = 1;
    std::int64_t length = 0;     // frames
    double suggestedRootHz = 0.0; // from the file's root-note metadata, or 0
    struct Onset
    {
        std::int64_t frame = 0;
        float strength = 0.0f; // how far the energy rise stands out (standard deviations)
    };
    std::vector<Onset> onsets; // onset candidates, found at load time (slicing by transient)

    const float* channel(int c) const noexcept
    {
        return storage[static_cast<std::size_t>(c < numChannels ? c : 0)].data() + padding();
    }
    std::size_t memoryBytes() const noexcept { return (storage[0].size() + storage[1].size()) * sizeof(float); }

    /** Builds a sample from channels in memory (tests, generated samples). */
    static std::shared_ptr<SampleData> fromChannels(const std::vector<std::vector<float>>& channels, double sampleRate,
                                                    juce::String name = {});

    std::vector<float> storage[2];
};

using SamplePtr = std::shared_ptr<const SampleData>;

/** Finds onset candidates in a sample: frames where the short-term energy rises sharply, each with
    how far its rise stands out from the sample's typical rise. */
std::vector<SampleData::Onset> findOnsets(const SampleData& sample);

/** The onset strength a sensitivity (0..1, higher finds more) asks for. */
inline float onsetThreshold(float sensitivity) noexcept
{
    return 3.0f - 2.5f * juce::jlimit(0.0f, 1.0f, sensitivity);
}

/** Every sample in use, loaded once (docs/04-m3-devices.md, "Sampler": identical files are shared
    through a reference-counted pool). Files are decoded on background threads; the callback runs on
    the message thread. A sample stays in the pool while any device holds it. */
class SamplePool
{
public:
    using Callback = std::function<void(SamplePtr sample, const juce::String& error)>;

    static SamplePool& shared();

    SamplePool();
    ~SamplePool();

    /** Message thread. Loads the file in the background (or finds it already loaded) and calls
        `done` on the message thread. */
    void request(const juce::File& file, Callback done);

    /** Any thread except the audio thread: loads now, or returns the copy already in the pool. */
    SamplePtr loadNow(const juce::File& file, juce::String* error = nullptr);

    /** Samples currently alive in the pool (tests). */
    int numLoaded() const;
    /** Waits for background loads to finish (tests, shutdown). */
    void waitUntilIdle();
    /** Message thread: runs the callbacks of finished loads. Called automatically through the
        message loop; tests without one call it directly. */
    void deliverFinished();

    static juce::AudioFormatManager& formats();

private:
    static juce::String keyFor(const juce::File& file);
    SamplePtr findLoaded(const juce::String& key) const;
    SamplePtr decode(const juce::File& file, juce::String& error) const;

    struct Finished
    {
        Callback done;
        SamplePtr sample;
        juce::String error;
    };

    mutable std::mutex lock;
    std::vector<Finished> finished; // guarded by lock
    std::map<juce::String, std::weak_ptr<const SampleData>> loaded;
    juce::ThreadPool loaders{2};
    std::shared_ptr<bool> alive = std::make_shared<bool>(true);
};

} // namespace lattice
