#pragma once

// Files the library and browser tests write: WAVs with or without a root note, tunings, a folder
// that deletes itself.

#include <juce_audio_formats/juce_audio_formats.h>

#include <cmath>
#include <unordered_map>

namespace lattice::test
{

/** A temporary folder, deleted (with everything in it) when it goes. Its name is unique to it, so
    test processes run in parallel (ctest -j) never share one: a name that's merely free when asked
    for could be taken by another process before it's made. */
struct TempFolder
{
    explicit TempFolder(const juce::String& name = "lattice-test")
        : folder(juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile(name + "-" + juce::Uuid().toString()))
    {
        folder.createDirectory();
    }
    ~TempFolder() { folder.deleteRecursively(); }
    TempFolder(const TempFolder&) = delete;
    TempFolder& operator=(const TempFolder&) = delete;

    juce::File operator/(const juce::String& relative) const { return folder.getChildFile(relative); }

    juce::File folder;
};

/** A 16-bit WAV of a sine. rootNote >= 0 writes it in the smpl chunk (MidiUnityNote). */
inline bool writeWav(const juce::File& file, double seconds, double sampleRate = 48000.0, int channels = 1,
                     int rootNote = -1, double hz = 220.0)
{
    file.getParentDirectory().createDirectory();
    file.deleteFile();
    std::unique_ptr<juce::OutputStream> out = std::make_unique<juce::FileOutputStream>(file);
    std::unordered_map<juce::String, juce::String> meta;
    if (rootNote >= 0)
        meta = {{"MidiUnityNote", juce::String(rootNote)}, {"NumSampleLoops", "0"}};
    juce::WavAudioFormat wav;
    auto options = juce::AudioFormatWriterOptions{}
                       .withSampleRate(sampleRate)
                       .withNumChannels(channels)
                       .withBitsPerSample(16);
    if (rootNote >= 0)
        options = options.withMetadataValues(meta);
    auto writer = wav.createWriterFor(out, options);
    if (writer == nullptr)
        return false;
    const auto frames = static_cast<int>(seconds * sampleRate);
    juce::AudioBuffer<float> buffer(channels, frames);
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < frames; ++i)
            buffer.setSample(c, i, 0.5f * static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * hz * i / sampleRate)));
    return writer->writeFromAudioSampleBuffer(buffer, 0, frames);
}

/** A table in the Colundi text format: 128 rising frequencies from `base` Hz. */
inline juce::String colundiTable(double base = 30.0)
{
    juce::String text = "#\tFreq\tCents\tRatio\tName\n";
    for (int i = 0; i < 128; ++i)
        text << "\t" << i << "\t" << juce::String(base * std::pow(2.0, i / 17.0), 3) << "Hz\t0\t1\t1\n";
    return text;
}

/** A Scala scale: five equal steps to the octave. */
inline juce::String scalaScale(const juce::String& description = "Five equal steps")
{
    return "! test.scl\n" + description + "\n 5\n!\n 240.0\n 480.0\n 720.0\n 960.0\n 2/1\n";
}

} // namespace lattice::test
