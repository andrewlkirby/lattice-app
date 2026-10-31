#include "library/WaveformPeaks.h"

#include "devices/samples/SamplePool.h"

#include <algorithm>
#include <cmath>

namespace lattice::library
{

namespace
{
constexpr juce::uint32 fileMagic = 0x4c504b31; // "LPK1"

bool cancelledNow(const WaveformPeaks::Cancelled& cancelled)
{
    return cancelled && cancelled();
}
} // namespace

void WaveformPeaks::addLevelsAbove()
{
    while (levels.back().low.size() > 512)
    {
        const auto& below = levels.back();
        Level up;
        up.framesPerBin = below.framesPerBin * 4;
        const std::size_t n = (below.low.size() + 3) / 4;
        up.low.assign(n, 0.0f);
        up.high.assign(n, 0.0f);
        for (std::size_t i = 0; i < below.low.size(); ++i)
        {
            up.low[i / 4] = std::min(up.low[i / 4], below.low[i]);
            up.high[i / 4] = std::max(up.high[i / 4], below.high[i]);
        }
        levels.push_back(std::move(up));
    }
}

std::shared_ptr<const WaveformPeaks> WaveformPeaks::build(const SampleData& sample, const Cancelled& cancelled)
{
    auto p = std::make_shared<WaveformPeaks>();
    p->length = sample.length;
    p->sampleRate = sample.sampleRate;
    Level base;
    const auto bins = static_cast<std::size_t>((sample.length + baseFrames - 1) / baseFrames);
    base.low.assign(bins, 0.0f);
    base.high.assign(bins, 0.0f);
    for (std::size_t b = 0; b < bins; ++b)
    {
        if ((b & 4095) == 0 && cancelledNow(cancelled))
            return nullptr;
        float lo = 0.0f, hi = 0.0f;
        const auto first = static_cast<std::int64_t>(b) * baseFrames;
        const auto last = std::min<std::int64_t>(sample.length, first + baseFrames);
        for (int c = 0; c < sample.numChannels; ++c)
        {
            const float* data = sample.channel(c);
            for (auto f = first; f < last; ++f)
                lo = std::min(lo, data[f]), hi = std::max(hi, data[f]);
        }
        base.low[b] = lo;
        base.high[b] = hi;
    }
    p->levels.push_back(std::move(base));
    p->addLevelsAbove();
    return p;
}

std::shared_ptr<const WaveformPeaks> WaveformPeaks::build(juce::AudioFormatReader& reader, const Cancelled& cancelled)
{
    auto p = std::make_shared<WaveformPeaks>();
    p->length = reader.lengthInSamples;
    p->sampleRate = reader.sampleRate;
    const int channels = juce::jlimit(1, 2, static_cast<int>(reader.numChannels));
    Level base;
    const auto bins = static_cast<std::size_t>((p->length + baseFrames - 1) / baseFrames);
    base.low.assign(bins, 0.0f);
    base.high.assign(bins, 0.0f);
    constexpr int block = baseFrames * 1024;
    juce::AudioBuffer<float> buffer(channels, block);
    for (std::int64_t at = 0; at < p->length; at += block)
    {
        if (cancelledNow(cancelled))
            return nullptr;
        const int n = static_cast<int>(std::min<std::int64_t>(block, p->length - at));
        if (!reader.read(&buffer, 0, n, at, true, channels > 1))
            buffer.clear();
        for (int i = 0; i < n; i += baseFrames)
        {
            float lo = 0.0f, hi = 0.0f;
            const int end = std::min(n, i + baseFrames);
            for (int c = 0; c < channels; ++c)
            {
                const float* data = buffer.getReadPointer(c);
                for (int f = i; f < end; ++f)
                    lo = std::min(lo, data[f]), hi = std::max(hi, data[f]);
            }
            const auto b = static_cast<std::size_t>((at + i) / baseFrames);
            base.low[b] = lo;
            base.high[b] = hi;
        }
    }
    p->levels.push_back(std::move(base));
    p->addLevelsAbove();
    return p;
}

std::pair<float, float> WaveformPeaks::range(double from, double to) const
{
    if (levels.empty() || levels.front().low.empty())
        return {0.0f, 0.0f};
    // The coarsest level that still gives at least one bin over the range.
    const Level* level = &levels.front();
    for (const auto& l : levels)
        if (l.framesPerBin <= (to - from))
            level = &l;
    const auto n = static_cast<std::int64_t>(level->low.size());
    auto b0 = juce::jlimit<std::int64_t>(0, n - 1, static_cast<std::int64_t>(from / level->framesPerBin));
    const auto b1 = juce::jlimit<std::int64_t>(b0, n - 1, static_cast<std::int64_t>(std::ceil(to / level->framesPerBin)) - 1);
    float lo = 0.0f, hi = 0.0f;
    for (; b0 <= b1; ++b0)
        lo = std::min(lo, level->low[static_cast<std::size_t>(b0)]), hi = std::max(hi, level->high[static_cast<std::size_t>(b0)]);
    return {lo, hi};
}

void WaveformPeaks::write(juce::OutputStream& out) const
{
    // Only the base level: the others are rebuilt from it in a moment.
    out.writeInt(static_cast<int>(fileMagic));
    out.writeInt64(length);
    out.writeDouble(sampleRate);
    const auto& base = levels.empty() ? Level{} : levels.front();
    out.writeInt64(static_cast<juce::int64>(base.low.size()));
    for (std::size_t i = 0; i < base.low.size(); ++i)
    {
        // 16 bits a value is more than a screen shows.
        out.writeShort(static_cast<short>(juce::jlimit(-32767, 32767, juce::roundToInt(base.low[i] * 32767.0f))));
        out.writeShort(static_cast<short>(juce::jlimit(-32767, 32767, juce::roundToInt(base.high[i] * 32767.0f))));
    }
}

std::shared_ptr<const WaveformPeaks> WaveformPeaks::read(juce::InputStream& in)
{
    if (static_cast<juce::uint32>(in.readInt()) != fileMagic)
        return nullptr;
    auto p = std::make_shared<WaveformPeaks>();
    p->length = in.readInt64();
    p->sampleRate = in.readDouble();
    const auto bins = in.readInt64();
    if (p->length < 0 || bins < 0 || bins != (p->length + baseFrames - 1) / baseFrames || in.getNumBytesRemaining() < bins * 4)
        return nullptr;
    Level base;
    base.low.resize(static_cast<std::size_t>(bins));
    base.high.resize(static_cast<std::size_t>(bins));
    for (std::size_t i = 0; i < base.low.size(); ++i)
    {
        base.low[i] = static_cast<float>(in.readShort()) / 32767.0f;
        base.high[i] = static_cast<float>(in.readShort()) / 32767.0f;
    }
    p->levels.push_back(std::move(base));
    p->addLevelsAbove();
    return p;
}

// --- PeakCache ------------------------------------------------------------------------------------------

PeakCache::PeakCache(juce::File folder) : dir(std::move(folder)) {}

juce::File PeakCache::fileFor(const juce::File& audio) const
{
    const auto identity = audio.getFullPathName() + "|" + juce::String(audio.getSize()) + "|" +
                          juce::String(audio.getLastModificationTime().toMilliseconds());
    return dir.getChildFile(juce::String::toHexString(identity.hashCode64()) + ".peaks");
}

std::shared_ptr<const WaveformPeaks> PeakCache::load(const juce::File& audio) const
{
    if (dir == juce::File())
        return nullptr;
    const auto file = fileFor(audio);
    std::shared_ptr<const WaveformPeaks> peaks;
    {
        juce::FileInputStream in(file);
        if (!in.openedOk())
            return nullptr;
        peaks = WaveformPeaks::read(in);
    }
    // Its date says when it was last used (access times aren't kept everywhere): prune() goes by it.
    if (peaks != nullptr)
        file.setLastModificationTime(juce::Time::getCurrentTime());
    return peaks;
}

void PeakCache::store(const juce::File& audio, const WaveformPeaks& peaks) const
{
    if (dir == juce::File() || !dir.createDirectory())
        return;
    // Written beside, then moved into place: a reader never sees half a file.
    const auto target = fileFor(audio);
    const juce::TemporaryFile temp(target);
    {
        juce::FileOutputStream out(temp.getFile());
        if (!out.openedOk())
            return;
        peaks.write(out);
    }
    temp.overwriteTargetFileWithTemporary();
}

std::shared_ptr<const WaveformPeaks> PeakCache::get(const juce::File& audio, const WaveformPeaks::Cancelled& cancelled) const
{
    if (auto cached = load(audio))
        return cached;
    std::unique_ptr<juce::AudioFormatReader> reader(SamplePool::formats().createReaderFor(audio));
    if (reader == nullptr)
        return nullptr;
    auto built = WaveformPeaks::build(*reader, cancelled);
    if (built != nullptr)
        store(audio, *built);
    return built;
}

void PeakCache::prune(std::int64_t maxBytes) const
{
    auto files = dir.findChildFiles(juce::File::findFiles, false, "*.peaks");
    std::int64_t total = 0;
    for (const auto& f : files)
        total += f.getSize();
    if (total <= maxBytes)
        return;
    std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b) {
        return a.getLastModificationTime() < b.getLastModificationTime();
    });
    for (const auto& f : files)
    {
        if (total <= maxBytes)
            break;
        total -= f.getSize();
        f.deleteFile();
    }
}

} // namespace lattice::library
