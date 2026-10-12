#include "devices/test/TuningTestSynth.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace lattice
{
namespace
{
constexpr float voiceLevel = 0.2f;       // at velocity 127; 16 voices stay below clipping in practice
constexpr double softSawCutoffHz = 5000.0;

/** PolyBLEP correction for a saw step at phase 0 (t and dt in cycles). */
double polyBlep(double t, double dt) noexcept
{
    if (t < dt)
    {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt)
    {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}
} // namespace

TuningTestSynth::TuningTestSynth(const TrackTuning& t) : tuning(t)
{
}

void TuningTestSynth::prepare(double newSampleRate)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    sampleRate = newSampleRate;
    const juce::ADSR::Parameters params{0.005f, 0.25f, 0.7f, 0.3f};
    for (auto& v : voices)
    {
        v = Voice{};
        v.envelope.setSampleRate(sampleRate);
        v.envelope.setParameters(params);
    }
    bend = 0.0f;
}

int TuningTestSynth::activeVoiceCount() const noexcept
{
    return static_cast<int>(std::count_if(voices.begin(), voices.end(), [](const Voice& v) { return v.envelope.isActive(); }));
}

void TuningTestSynth::render(float* out, int numSamples, const TimedMidi* events, int numEvents) noexcept
{
    int pos = 0;
    int next = 0;
    while (pos < numSamples)
    {
        while (next < numEvents && events[next].sampleOffset <= pos)
            handleEvent(events[next++]);

        const int end = next < numEvents ? std::clamp(events[next].sampleOffset, pos + 1, numSamples) : numSamples;
        renderVoices(out + pos, end - pos);
        pos = end;
    }

    // Events stamped at or beyond the end of the block take effect for the next one.
    while (next < numEvents)
        handleEvent(events[next++]);
}

void TuningTestSynth::handleEvent(const TimedMidi& e) noexcept
{
    if (e.size < 1)
        return;
    const int status = e.bytes[0] & 0xf0;
    const int data1 = e.size > 1 ? e.bytes[1] : 0;
    const int data2 = e.size > 2 ? e.bytes[2] : 0;

    switch (status)
    {
    case 0x90:
        if (data2 > 0)
            noteOn(data1, data2);
        else
            noteOff(data1);
        break;
    case 0x80:
        noteOff(data1);
        break;
    case 0xe0:
        bend = std::clamp(static_cast<float>(((data2 << 7) | data1) - 8192) / 8192.0f, -1.0f, 1.0f);
        break;
    case 0xb0:
        if (data1 == 120 || data1 == 123) // all sound off / all notes off
            allNotesOff();
        break;
    default:
        break;
    }
}

void TuningTestSynth::noteOn(int note, int velocity) noexcept
{
    const double hz = tuning.hzFor(note, bend);
    if (hz <= 0.0)
        return; // unmapped, or above the playable range under the Mute policy

    Voice* chosen = nullptr;
    for (auto& v : voices)
    {
        if (!v.envelope.isActive())
        {
            chosen = &v;
            break;
        }
    }
    if (chosen == nullptr)
        chosen = &*std::min_element(voices.begin(), voices.end(),
                                    [](const Voice& a, const Voice& b) { return a.startedAt < b.startedAt; });

    auto& v = *chosen;
    v.note = note;
    v.held = true;
    v.hz = hz;
    v.gain = voiceLevel * static_cast<float>(velocity) / 127.0f;
    v.phase = 0.0;
    v.sawState = 0.0f;
    v.startedAt = ++noteCounter;
    v.envelope.noteOn();
    lastNote.store(note, std::memory_order_relaxed);
}

void TuningTestSynth::noteOff(int note) noexcept
{
    for (auto& v : voices)
    {
        if (v.held && v.note == note)
        {
            v.held = false;
            v.envelope.noteOff();
        }
    }
}

void TuningTestSynth::allNotesOff() noexcept
{
    for (auto& v : voices)
    {
        if (v.envelope.isActive())
        {
            v.held = false;
            v.envelope.noteOff();
        }
    }
}

void TuningTestSynth::renderVoices(float* out, int numSamples) noexcept
{
    std::fill(out, out + numSamples, 0.0f);

    constexpr double twoPi = 2.0 * std::numbers::pi;
    const float mix = sawMix.load(std::memory_order_relaxed);
    const float sawCoeff = static_cast<float>(1.0 - std::exp(-twoPi * softSawCutoffHz / sampleRate));

    for (auto& v : voices)
    {
        if (!v.envelope.isActive())
            continue;

        // Follow tuning changes and pitch bend once per segment. A note bent out of range keeps
        // its last pitch rather than cutting off.
        if (const double hz = tuning.hzFor(v.note, bend); hz > 0.0)
            v.hz = hz;

        const double inc = v.hz / sampleRate;
        // Fade the saw out as its fundamental nears Nyquist, where even PolyBLEP aliases badly.
        const float sawWeight = mix * static_cast<float>(std::clamp((0.25 - inc) / 0.15, 0.0, 1.0));
        const float sineWeight = 1.0f - 0.5f * sawWeight;

        for (int i = 0; i < numSamples; ++i)
        {
            const double saw = 2.0 * v.phase - 1.0 - polyBlep(v.phase, inc);
            v.sawState += sawCoeff * (static_cast<float>(saw) - v.sawState);
            const float sample = sineWeight * static_cast<float>(std::sin(twoPi * v.phase)) + 0.5f * sawWeight * v.sawState;
            out[i] += v.gain * v.envelope.getNextSample() * sample;

            v.phase += inc;
            if (v.phase >= 1.0)
                v.phase -= 1.0;
        }
    }
}

} // namespace lattice
