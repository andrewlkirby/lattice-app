#include "devices/samples/SamplePool.h"

#include "core/RealtimeCheck.h"
#include "devices/dsp/SincResampler.h"

#include <cmath>
#include <numeric>

namespace lattice
{

int SampleData::padding()
{
    static const int p = dsp::SincKernelBank::get().maxTaps() / 2 + 8;
    return p;
}

std::shared_ptr<SampleData> SampleData::fromChannels(const std::vector<std::vector<float>>& channels, double sr,
                                                     juce::String sampleName)
{
    auto s = std::make_shared<SampleData>();
    s->name = std::move(sampleName);
    s->sampleRate = sr;
    s->numChannels = juce::jlimit(1, 2, static_cast<int>(channels.size()));
    s->length = channels.empty() ? 0 : static_cast<std::int64_t>(channels[0].size());
    const auto pad = static_cast<std::size_t>(padding());
    for (int c = 0; c < s->numChannels; ++c)
    {
        auto& store = s->storage[static_cast<std::size_t>(c)];
        store.assign(static_cast<std::size_t>(s->length) + 2 * pad, 0.0f);
        std::copy(channels[static_cast<std::size_t>(c)].begin(), channels[static_cast<std::size_t>(c)].end(),
                  store.begin() + static_cast<std::ptrdiff_t>(pad));
    }
    s->onsets = findOnsets(*s);
    return s;
}

std::vector<SampleData::Onset> findOnsets(const SampleData& sample)
{
    // Log energy in short frames; a candidate is a frame whose rise over the previous frame is a
    // local peak of at least 3 dB on a frame loud enough to matter. Its strength is how many
    // standard deviations the rise is above the sample's mean rise.
    constexpr int frame = 512;
    constexpr int hop = 256;
    std::vector<SampleData::Onset> onsets;
    if (sample.length < frame * 2)
        return onsets;

    std::vector<double> energy;
    for (std::int64_t start = 0; start + frame <= sample.length; start += hop)
    {
        double e = 0.0;
        for (int c = 0; c < sample.numChannels; ++c)
        {
            const float* x = sample.channel(c) + start;
            for (int i = 0; i < frame; ++i)
                e += static_cast<double>(x[i]) * x[i];
        }
        energy.push_back(10.0 * std::log10(e / frame + 1e-12));
    }
    std::vector<double> rise(energy.size(), 0.0);
    for (std::size_t i = 1; i < energy.size(); ++i)
        rise[i] = std::max(0.0, energy[i] - energy[i - 1]);
    const double mean = std::accumulate(rise.begin(), rise.end(), 0.0) / static_cast<double>(rise.size());
    double variance = 0.0;
    for (const double r : rise)
        variance += (r - mean) * (r - mean);
    const double deviation = std::max(1e-6, std::sqrt(variance / static_cast<double>(rise.size())));
    const auto minGap = static_cast<std::int64_t>(0.05 * sample.sampleRate);

    for (std::size_t i = 1; i + 1 < rise.size(); ++i)
    {
        if (rise[i] < 3.0 || rise[i] < rise[i - 1] || rise[i] < rise[i + 1] || energy[i] < -50.0)
            continue;
        const auto at = static_cast<std::int64_t>(i) * hop;
        const auto strength = static_cast<float>((rise[i] - mean) / deviation);
        if (!onsets.empty() && at - onsets.back().frame < minGap)
        {
            if (strength > onsets.back().strength)
                onsets.back() = {at, strength};
            continue;
        }
        onsets.push_back({at, strength});
    }
    return onsets;
}

SamplePool& SamplePool::shared()
{
    static SamplePool pool;
    return pool;
}

SamplePool::SamplePool() = default;

SamplePool::~SamplePool()
{
    *alive = false;
    loaders.removeAllJobs(true, 10000);
}

juce::AudioFormatManager& SamplePool::formats()
{
    static juce::AudioFormatManager manager;
    static std::once_flag once;
    std::call_once(once, [] { manager.registerBasicFormats(); });
    return manager;
}

juce::String SamplePool::keyFor(const juce::File& file)
{
    return file.getFullPathName() + "|" + juce::String(file.getSize()) + "|" +
           juce::String(file.getLastModificationTime().toMilliseconds());
}

SamplePtr SamplePool::findLoaded(const juce::String& key) const
{
    const std::scoped_lock guard(lock);
    const auto it = loaded.find(key);
    return it != loaded.end() ? it->second.lock() : nullptr;
}

int SamplePool::numLoaded() const
{
    const std::scoped_lock guard(lock);
    int n = 0;
    for (const auto& [key, weak] : loaded)
        if (!weak.expired())
            ++n;
    return n;
}

SamplePtr SamplePool::decode(const juce::File& file, juce::String& error) const
{
    std::unique_ptr<juce::AudioFormatReader> reader(formats().createReaderFor(file));
    if (reader == nullptr)
    {
        error = file.existsAsFile() ? "Unsupported audio file: " + file.getFileName()
                                    : "File not found: " + file.getFullPathName();
        return nullptr;
    }
    const auto frames = static_cast<int>(std::min<juce::int64>(reader->lengthInSamples, std::numeric_limits<int>::max() / 2));
    const int channels = juce::jlimit(1, 2, static_cast<int>(reader->numChannels));
    juce::AudioBuffer<float> buffer(channels, frames);
    reader->read(&buffer, 0, frames, 0, true, channels > 1);

    std::vector<std::vector<float>> data(static_cast<std::size_t>(channels));
    for (int c = 0; c < channels; ++c)
        data[static_cast<std::size_t>(c)].assign(buffer.getReadPointer(c), buffer.getReadPointer(c) + frames);
    auto sample = SampleData::fromChannels(data, reader->sampleRate, file.getFileNameWithoutExtension());
    sample->path = file.getFullPathName();
    const auto unity = reader->metadataValues.getValue("MidiUnityNote", {});
    if (unity.isNotEmpty())
        sample->suggestedRootHz = 440.0 * std::exp2((unity.getIntValue() - 69) / 12.0);
    return sample;
}

SamplePtr SamplePool::loadNow(const juce::File& file, juce::String* error)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    const auto key = keyFor(file);
    if (auto existing = findLoaded(key))
        return existing;
    juce::String message;
    auto sample = decode(file, message);
    if (error != nullptr)
        *error = message;
    if (sample == nullptr)
        return nullptr;
    const std::scoped_lock guard(lock);
    if (auto raced = loaded[key].lock()) // another thread loaded it meanwhile: share that one
        return raced;
    loaded[key] = sample;
    return sample;
}

void SamplePool::request(const juce::File& file, Callback done)
{
    if (auto existing = findLoaded(keyFor(file)))
    {
        done(existing, {});
        return;
    }
    loaders.addJob([this, file, done = std::move(done), token = alive] {
        juce::String error;
        auto sample = loadNow(file, &error);
        {
            const std::scoped_lock guard(lock);
            finished.push_back({done, std::move(sample), error});
        }
        juce::MessageManager::callAsync([this, token] {
            if (*token)
                deliverFinished();
        });
    });
}

void SamplePool::deliverFinished()
{
    std::vector<Finished> ready;
    {
        const std::scoped_lock guard(lock);
        ready.swap(finished);
    }
    for (auto& f : ready)
        f.done(f.sample, f.error);
}

void SamplePool::waitUntilIdle()
{
    while (loaders.getNumJobs() > 0)
        juce::Thread::sleep(1);
}

} // namespace lattice
