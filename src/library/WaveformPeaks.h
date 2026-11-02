#pragma once

#include <juce_audio_formats/juce_audio_formats.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace lattice
{
struct SampleData;
}

namespace lattice::library
{

/** A sample's peaks at several zoom levels (docs/05-m4-ui.md, "Rendering and performance"): the
    lowest and highest value of each run of frames, 64 frames a run, then 4 times longer at each level
    up, so a view of any zoom reads a few thousand values at most. Built in the background, from a
    decoded sample (the Sampler) or straight from the file (the browser's preview), and cached on
    disk (PeakCache). */
struct WaveformPeaks
{
    static constexpr int baseFrames = 64;
    struct Level
    {
        int framesPerBin = baseFrames;
        std::vector<float> low, high;
    };
    std::vector<Level> levels;
    std::int64_t length = 0; // frames
    double sampleRate = 0.0;

    using Cancelled = std::function<bool()>;
    static std::shared_ptr<const WaveformPeaks> build(const SampleData& sample, const Cancelled& cancelled);
    /** Reads the file a block at a time, so nothing is held but the peaks. */
    static std::shared_ptr<const WaveformPeaks> build(juce::AudioFormatReader& reader, const Cancelled& cancelled);

    /** The lowest and highest value over frames [from, to). */
    std::pair<float, float> range(double from, double to) const;

    void write(juce::OutputStream& out) const;
    static std::shared_ptr<const WaveformPeaks> read(juce::InputStream& in);

private:
    void addLevelsAbove();
};

/** Peaks kept on disk, one file per audio file (named by a hash of its path, size and date, so a
    changed file gets new peaks). Any thread. */
class PeakCache
{
public:
    explicit PeakCache(juce::File folder);
    const juce::File& folder() const noexcept { return dir; }

    std::shared_ptr<const WaveformPeaks> load(const juce::File& audio) const;
    void store(const juce::File& audio, const WaveformPeaks& peaks) const;

    /** From the cache, or built from the file and stored. Null if the file can't be read. */
    std::shared_ptr<const WaveformPeaks> get(const juce::File& audio, const WaveformPeaks::Cancelled& cancelled = {}) const;

    /** Deletes the files used longest ago (stored or loaded) until the cache holds at most maxBytes. */
    void prune(std::int64_t maxBytes) const;

    juce::File fileFor(const juce::File& audio) const;

private:
    juce::File dir;
};

} // namespace lattice::library
