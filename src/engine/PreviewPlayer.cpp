#include "engine/PreviewPlayer.h"

#include <algorithm>
#include <cmath>

namespace lattice
{

std::uint64_t PreviewPlayer::play(SamplePtr sound, bool syncToBar)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    auto s = std::make_shared<Sound>();
    s->generation = nextGeneration++;
    s->sample = std::move(sound);
    s->sync = syncToBar;
    latest.store(s.get(), std::memory_order_release);
    const auto generation = s->generation;
    alive.push_back(std::move(s));
    collectGarbage();
    return generation;
}

void PreviewPlayer::collectGarbage()
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    // Everything older than what the audio thread still uses can go; the newest always stays.
    const auto oldestInUse = acked.load(std::memory_order_acquire);
    const auto* newest = latest.load(std::memory_order_relaxed);
    alive.erase(std::remove_if(alive.begin(), alive.end(),
                               [&](const std::shared_ptr<const Sound>& s) {
                                   return s.get() != newest && s->generation < oldestInUse;
                               }),
                alive.end());
}

void PreviewPlayer::prepare(double rate) noexcept
{
    sampleRate = rate > 0.0 ? rate : 48000.0;
    current = {};
    fading = {};
    lastGain = gain.load(std::memory_order_relaxed);
}

void PreviewPlayer::renderVoice(Voice& v, float* left, float* right, int numSamples, float g0, float g1, bool fadingOut) noexcept
{
    const auto* sample = v.sound->sample.get();
    const double step = sample->sampleRate / sampleRate;
    const float* l = sample->channel(0);
    const float* r = sample->channel(sample->numChannels > 1 ? 1 : 0);
    const auto length = static_cast<double>(sample->length);
    int i = 0;
    if (v.wait > 0)
    {
        const auto skip = static_cast<int>(std::min<std::int64_t>(v.wait, numSamples));
        v.wait -= skip;
        i = skip;
    }
    const float gainStep = (g1 - g0) / static_cast<float>(std::max(1, numSamples));
    for (; i < numSamples && v.position < length; ++i)
    {
        if (fadingOut && v.fade <= 0)
            break;
        const auto at = static_cast<std::int64_t>(v.position);
        const auto t = static_cast<float>(v.position - static_cast<double>(at));
        // The channels are padded with silence, so at + 1 is always readable.
        float g = g0 + gainStep * static_cast<float>(i);
        if (fadingOut)
            g *= static_cast<float>(v.fade--) / static_cast<float>(fadeFrames);
        left[i] += g * (l[at] + t * (l[at + 1] - l[at]));
        right[i] += g * (r[at] + t * (r[at + 1] - r[at]));
        v.position += step;
    }
}

void PreviewPlayer::render(float* left, float* right, int numSamples, double samplesToNextBar) noexcept
{
    const auto* next = latest.load(std::memory_order_acquire);
    if (next != current.sound)
    {
        // The old sound fades out while the new one starts (if one is still fading, it's cut: two
        // changes inside a few milliseconds).
        fading = current;
        fading.fade = fading.sound != nullptr && fading.sound->sample != nullptr && fading.wait == 0 ? fadeFrames : 0;
        if (fading.fade == 0)
            fading.sound = nullptr;
        current = {};
        current.sound = next;
        if (next != nullptr && next->sync && samplesToNextBar >= 0.0)
            current.wait = static_cast<std::int64_t>(std::llround(samplesToNextBar));
    }

    const float target = gain.load(std::memory_order_relaxed);
    const float g0 = lastGain;
    lastGain = target;

    if (fading.sound != nullptr)
    {
        renderVoice(fading, left, right, numSamples, g0, target, true);
        if (fading.fade <= 0 || fading.position >= static_cast<double>(fading.sound->sample->length))
            fading.sound = nullptr;
    }
    bool sounding = false;
    if (current.sound != nullptr && current.sound->sample != nullptr)
    {
        const auto length = static_cast<double>(current.sound->sample->length);
        if (current.position < length)
        {
            renderVoice(current, left, right, numSamples, g0, target, false);
            sounding = current.position < length || current.wait > 0;
        }
    }
    playingNow.store(sounding, std::memory_order_relaxed);
    frame.store(sounding && current.wait == 0 ? static_cast<std::int64_t>(current.position) : -1, std::memory_order_relaxed);

    // Tell the message thread which sounds are still in use.
    const auto* oldest = fading.sound != nullptr ? fading.sound : current.sound;
    acked.store(oldest != nullptr ? oldest->generation : (next != nullptr ? next->generation : 0), std::memory_order_release);
}

} // namespace lattice
