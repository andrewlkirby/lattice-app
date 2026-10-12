#include "devices/instruments/Sampler.h"

#include "devices/dsp/SyncRates.h"

#include <algorithm>
#include <cmath>

namespace lattice
{
namespace
{
using P = SamplerDevice::Param;

juce::String percent(float v)
{
    return juce::String(v * 100.0f, 1) + " %";
}

juce::String panText(float v)
{
    const int p = juce::roundToInt(std::abs(v) * 50.0f);
    return p == 0 ? juce::String("C") : juce::String(p) + (v < 0.0f ? "L" : "R");
}

ParameterSpec level01(const char* id, const char* name, float def)
{
    return ParameterSpec::continuous(id, name, 0.0f, 1.0f, def, ParamUnit::Percent).withFormatter(percent);
}

ParameterSpec time(const char* id, const char* name, float def, float max = 20000.0f)
{
    return ParameterSpec::continuous(id, name, 0.0f, max, def, ParamUnit::Milliseconds).withCentre(200.0f);
}

std::vector<ParameterSpec> makeSpecs()
{
    std::vector<ParameterSpec> s(static_cast<std::size_t>(P::numParams));
    const auto set = [&](int index, ParameterSpec spec, const char* group) {
        spec.group = group;
        s[static_cast<std::size_t>(index)] = std::move(spec);
    };
    set(P::root,
        ParameterSpec::continuous("sample.root", "Root", 8.0f, 20000.0f, 261.6256f, ParamUnit::Hz).withCentre(440.0f),
        "Sample");
    set(P::transpose, ParameterSpec::integer("sample.transpose", "Transpose", -48, 48, 0, ParamUnit::TableSteps),
        "Sample");
    set(P::detune, ParameterSpec::continuous("sample.detune", "Detune", -100.0f, 100.0f, 0.0f, ParamUnit::Cents),
        "Sample");
    set(P::tuningMode, ParameterSpec::choice("sample.tuning", "Tuning", {"Track", "Fixed (12-TET)"}, 0), "Sample");
    set(P::start, level01("sample.start", "Start", 0.0f), "Sample");
    set(P::end, level01("sample.end", "End", 1.0f), "Sample");
    set(P::mode, ParameterSpec::choice("sample.mode", "Mode", {"Classic", "One-Shot", "Slice"}, 0), "Sample");
    set(P::quality, ParameterSpec::choice("sample.quality", "Quality", {"Sinc", "Draft"}, 0), "Sample");

    set(P::loopOn, ParameterSpec::toggle("loop.on", "Loop", false), "Loop");
    set(P::loopStart, level01("loop.start", "Loop Start", 0.0f), "Loop");
    set(P::loopLength, level01("loop.length", "Loop Length", 1.0f), "Loop");
    set(P::loopCrossfade, time("loop.crossfade", "Crossfade", 0.0f, 1000.0f), "Loop");

    set(P::fadeIn, time("oneshot.fadeIn", "Fade In", 0.0f, 2000.0f), "One-Shot");
    set(P::fadeOut, time("oneshot.fadeOut", "Fade Out", 0.0f, 2000.0f), "One-Shot");

    set(P::ampAttack, time("amp.attack", "Attack", 0.5f), "Amp Env");
    set(P::ampDecay, time("amp.decay", "Decay", 600.0f), "Amp Env");
    set(P::ampSustain, level01("amp.sustain", "Sustain", 1.0f), "Amp Env");
    set(P::ampRelease, time("amp.release", "Release", 50.0f), "Amp Env");
    set(P::ampVelocity, level01("amp.velocity", "Velocity", 0.5f), "Amp Env");

    set(P::filterOn, ParameterSpec::toggle("filter.on", "Filter", false), "Filter");
    set(P::filterType,
        ParameterSpec::choice("filter.type", "Filter Type",
                              {"Low-pass 12", "Low-pass 24", "High-pass 12", "High-pass 24", "Band-pass", "Notch"}, 0),
        "Filter");
    set(P::filterCutoff,
        ParameterSpec::continuous("filter.cutoff", "Cutoff", 20.0f, 20000.0f, 20000.0f, ParamUnit::Hz).withCentre(1000.0f),
        "Filter");
    set(P::filterResonance, level01("filter.resonance", "Resonance", 0.0f), "Filter");

    set(P::sliceBy, ParameterSpec::choice("slice.by", "Slice By", {"Transient", "Beat", "Manual"}, 0), "Slice");
    set(P::sliceSensitivity, level01("slice.sensitivity", "Sensitivity", 0.5f), "Slice");
    set(P::sliceDivision,
        ParameterSpec::choice("slice.division", "Division", dsp::SyncRates::names(), dsp::SyncRates::indexOf("1/16")),
        "Slice");
    set(P::sliceTempo,
        ParameterSpec::continuous("slice.tempo", "Sample Tempo", 20.0f, 999.0f, 120.0f).withFormatter([](float v) {
            return juce::String(v, 1) + " BPM";
        }),
        "Slice");

    set(P::polyphony, ParameterSpec::integer("voice.polyphony", "Voices", 1, SamplerDevice::maxVoices, 32), "Voice");
    set(P::choke, ParameterSpec::toggle("voice.choke", "Choke", false), "Voice");
    set(P::spread, level01("voice.spread", "Spread", 0.0f), "Voice");

    set(P::outVolume,
        ParameterSpec::continuous("out.volume", "Volume", -70.0f, 6.0f, -6.0f, ParamUnit::Decibels).withCentre(-12.0f),
        "Output");
    set(P::outPan, ParameterSpec::continuous("out.pan", "Pan", -1.0f, 1.0f, 0.0f).withFormatter(panText), "Output");
    return s;
}

float dbToGain(float db)
{
    return db <= -69.95f ? 0.0f : std::pow(10.0f, db / 20.0f);
}

void panGains(float pan, float& left, float& right)
{
    const float angle = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) * 0.25f * 3.14159265f;
    left = std::cos(angle);
    right = std::sin(angle);
}
} // namespace

SamplerDevice::SamplerDevice(const TrackTuning& t, SamplePool& p)
    : StudioDevice(typeId, "Sampler", DeviceKind::Instrument, makeSpecs()), tuning(t), pool(p)
{
    (void)dsp::SincKernelBank::get(); // built now, on this (message) thread, not on first play
    sources.publish(std::make_shared<Source>());
}

SamplerDevice::~SamplerDevice()
{
    *alive = false;
}

void SamplerDevice::prepare(double sr, int)
{
    for (auto& v : voices)
        v.amp.setSampleRate(sr);
    gainStep = static_cast<float>(1.0 / (0.02 * sr));
    sources.resetReader();
    active = nullptr;
    switching = false;
    reset();
}

void SamplerDevice::reset() noexcept
{
    for (auto& v : voices)
    {
        v.active = false;
        v.amp.reset();
        v.svf[0].reset();
        v.svf[1].reset();
    }
    sustainDown = false;
    sustained.fill(false);
    bend = 0.0f;
}

void SamplerDevice::releaseAllNotes() noexcept
{
    for (auto& v : voices)
        if (v.active && !v.stealing && !v.oneShot)
        {
            v.gate = false;
            v.amp.noteOff();
        }
    sustainDown = false;
    sustained.fill(false);
}

void SamplerDevice::choke() noexcept
{
    for (auto& v : voices)
        if (v.active && !v.stealing)
        {
            v.stealing = true;
            v.killStep = static_cast<float>(1.0 / (stealFadeSeconds * sampleRate()));
        }
}

int SamplerDevice::activeVoiceCount() const noexcept
{
    int n = 0;
    for (const auto& v : voices)
        if (v.active)
            ++n;
    return n;
}

// --- The sample (message thread) ---------------------------------------------------------------

void SamplerDevice::stateChanged(const juce::ValueTree& state)
{
    manualSlices.clear();
    for (const auto& token : juce::StringArray::fromTokens(state["slices"].toString(), ",", {}))
        if (token.trim().isNotEmpty())
            manualSlices.push_back(token.trim().getLargeIntValue());
    std::sort(manualSlices.begin(), manualSlices.end());

    const auto path = state["file"].toString();
    if (path != loadedFile && path != loadingFile)
    {
        loadFile(path);
        return;
    }
    // Same file, new slices: republish with them.
    auto next = std::make_shared<Source>();
    next->sample = currentSample();
    next->manualSlices = manualSlices;
    sources.publish(std::move(next));
}

void SamplerDevice::loadFile(const juce::String& path)
{
    error.clear();
    if (path.isEmpty())
    {
        loadingFile.clear();
        loadedFile.clear();
        setSample(nullptr);
        return;
    }
    loadingFile = path;
    pool.request(juce::File(path), [this, path, token = alive](SamplePtr sample, const juce::String& message) {
        if (!*token || loadingFile != path)
            return; // the device is gone, or another file was chosen meanwhile
        loadingFile.clear();
        loadedFile = path;
        error = message;
        setSample(std::move(sample));
    });
}

void SamplerDevice::setSample(SamplePtr sample)
{
    RT_ASSERT_NOT_AUDIO_THREAD();
    auto next = std::make_shared<Source>();
    next->sample = std::move(sample);
    next->manualSlices = manualSlices;
    sources.publish(std::move(next));
}

SamplePtr SamplerDevice::currentSample() const
{
    const auto s = sources.published();
    return s != nullptr ? s->sample : nullptr;
}

std::vector<std::int64_t> SamplerDevice::slicePoints() const
{
    const auto s = sources.published();
    if (s == nullptr || s->sample == nullptr)
        return {};
    std::array<std::int64_t, 512> points{};
    const int n = computeSlices(*s, points.data(), static_cast<int>(points.size()));
    return {points.begin(), points.begin() + n};
}

int SamplerDevice::computeSlices(const Source& source, std::int64_t* out, int capacity) const noexcept
{
    const auto& sample = *source.sample;
    const auto regionStart = static_cast<std::int64_t>(value(P::start) * static_cast<float>(sample.length));
    const auto regionEnd = static_cast<std::int64_t>(value(P::end) * static_cast<float>(sample.length));
    int n = 0;
    const auto add = [&](std::int64_t frame) {
        if (n < capacity && frame >= regionStart && frame < regionEnd && (n == 0 || frame > out[n - 1]))
            out[n++] = frame;
    };
    add(regionStart);
    switch (static_cast<SliceBy>(juce::roundToInt(value(P::sliceBy))))
    {
    case SliceBy::Transient:
    {
        const float threshold = onsetThreshold(value(P::sliceSensitivity));
        for (const auto& onset : sample.onsets)
            if (onset.strength >= threshold)
                add(onset.frame);
        break;
    }
    case SliceBy::Beat:
    {
        const double beats = dsp::SyncRates::beatsAt(juce::roundToInt(value(P::sliceDivision)));
        const double frames = beats * 60.0 / std::max(20.0f, value(P::sliceTempo)) * sample.sampleRate;
        for (double f = static_cast<double>(regionStart) + frames; f < static_cast<double>(regionEnd); f += frames)
            add(static_cast<std::int64_t>(std::llround(f)));
        break;
    }
    case SliceBy::Manual:
        for (const auto f : source.manualSlices)
            add(f);
        break;
    }
    return n;
}

// --- Notes (audio thread) ----------------------------------------------------------------------------

SamplerDevice::Voice* SamplerDevice::allocateVoice() noexcept
{
    const int polyphony = juce::jlimit(1, maxVoices, juce::roundToInt(value(P::polyphony)));
    int sounding = 0;
    Voice* idle = nullptr;
    for (auto& v : voices)
    {
        if (!v.active)
        {
            if (idle == nullptr)
                idle = &v;
        }
        else if (!v.stealing)
        {
            ++sounding;
        }
    }
    if (sounding >= polyphony)
    {
        Voice* victim = nullptr;
        for (auto& v : voices)
            if (v.active && !v.stealing &&
                (victim == nullptr || (!v.gate && victim->gate) || (v.gate == victim->gate && v.age < victim->age)))
                victim = &v;
        if (victim != nullptr)
        {
            victim->stealing = true;
            victim->killStep = static_cast<float>(1.0 / (stealFadeSeconds * sampleRate()));
        }
    }
    if (idle != nullptr)
        return idle;
    Voice* oldest = &voices[0];
    for (auto& v : voices)
        if (v.age < oldest->age)
            oldest = &v;
    return oldest;
}

double SamplerDevice::rateFor(const Voice& v) const noexcept
{
    const auto& sample = *active->object->sample;
    const double fileRatio = sample.sampleRate / sampleRate();
    if (!v.pitched)
        return fileRatio * std::exp2((value(P::transpose) + value(P::detune) / 100.0f) / 12.0);

    if (juce::roundToInt(value(P::tuningMode)) == 1)
    {
        // Fixed: 12-TET relative to C4, ignoring the track's tuning (Drum Rack pads).
        const double semitones = v.note - 60 + value(P::transpose) + value(P::detune) / 100.0 + 2.0 * bend;
        return fileRatio * std::exp2(semitones / 12.0);
    }
    double bendSteps = 0.0;
    double bendCents = 0.0;
    tuning.bendOffsets(bend, bendSteps, bendCents);
    const double hz = tuning.hzAt(v.note + value(P::transpose) + bendSteps, value(P::detune) + bendCents);
    const double rootHz = std::max(1.0f, value(P::root));
    return hz > 0.0 ? fileRatio * hz / rootHz : 0.0;
}

void SamplerDevice::noteOn(int note, int velocity) noexcept
{
    if (active == nullptr || active->object->sample == nullptr || switching)
        return;
    const auto& source = *active->object;
    const auto& sample = *source.sample;
    const auto mode = static_cast<Mode>(juce::roundToInt(value(P::mode)));

    double regionStart = value(P::start) * static_cast<double>(sample.length);
    double regionEnd = value(P::end) * static_cast<double>(sample.length);
    bool pitched = true;
    if (mode == Mode::Slice)
    {
        const int slices = computeSlices(source, sliceScratch.data(), static_cast<int>(sliceScratch.size()));
        const int index = note - firstSliceNote;
        if (index < 0 || index >= slices)
            return;
        regionStart = static_cast<double>(sliceScratch[static_cast<std::size_t>(index)]);
        if (index + 1 < slices)
            regionEnd = static_cast<double>(sliceScratch[static_cast<std::size_t>(index + 1)]);
        pitched = false;
    }
    if (regionEnd - regionStart < 2.0)
        return;

    if (value(P::choke) >= 0.5f)
        for (auto& other : voices)
            if (other.active && !other.stealing)
            {
                other.stealing = true;
                other.killStep = static_cast<float>(1.0 / (stealFadeSeconds * sampleRate()));
            }

    auto* v = allocateVoice();
    if (v == nullptr)
        return;
    v->active = true;
    v->gate = true;
    v->stealing = false;
    v->killGain = 1.0f;
    v->killStep = 0.0f;
    v->oneShot = mode == Mode::OneShot;
    v->note = note;
    v->velocity = static_cast<float>(velocity) / 127.0f;
    v->age = ++noteCounter;
    v->pitched = pitched;
    v->regionStart = regionStart;
    v->regionEnd = regionEnd;
    v->position = regionStart;
    v->played = 0;
    v->loop = mode == Mode::Classic && value(P::loopOn) >= 0.5f;
    const double span = regionEnd - regionStart;
    v->loopStart = regionStart + value(P::loopStart) * span;
    v->loopEnd = std::min(regionEnd, v->loopStart + std::max(0.001f, value(P::loopLength)) * span);
    if (v->loopEnd - v->loopStart < 2.0)
        v->loop = false;
    v->rate = rateFor(*v);

    randomSeed = randomSeed * 1664525u + 1013904223u;
    const float r = static_cast<float>(randomSeed >> 8) * (2.0f / 16777216.0f) - 1.0f;
    panGains(r * value(P::spread), v->panL, v->panR);

    dsp::Adsr::Settings env;
    if (mode == Mode::Classic || mode == Mode::Slice)
    {
        env.attackSeconds = std::max(0.0005f, value(P::ampAttack) * 0.001f);
        env.decaySeconds = value(P::ampDecay) * 0.001f;
        env.sustain = value(P::ampSustain);
        env.releaseSeconds = std::max(0.001f, value(P::ampRelease) * 0.001f);
    }
    else
    {
        // One-Shot: no envelope beyond the fades (applied while rendering).
        env.attackSeconds = 0.0005f;
        env.decaySeconds = 0.0f;
        env.sustain = 1.0f;
        env.releaseSeconds = 0.003f;
    }
    v->amp.setSettings(env);
    v->amp.reset();
    v->amp.noteOn();
    v->svf[0].reset();
    v->svf[1].reset();
}

void SamplerDevice::noteOff(int note) noexcept
{
    for (auto& v : voices)
    {
        if (!v.active || v.note != note || !v.gate || v.oneShot || v.stealing)
            continue;
        if (sustainDown)
        {
            sustained[static_cast<std::size_t>(note & 127)] = true;
            continue;
        }
        v.gate = false;
        v.amp.noteOff();
    }
}

void SamplerDevice::handleEvent(const TimedMidi& e) noexcept
{
    const int status = e.bytes[0] & 0xf0;
    switch (status)
    {
    case 0x90:
        if (e.bytes[2] > 0)
            noteOn(e.bytes[1], e.bytes[2]);
        else
            noteOff(e.bytes[1]);
        break;
    case 0x80:
        noteOff(e.bytes[1]);
        break;
    case 0xe0:
        bend = std::clamp(static_cast<float>(((e.bytes[2] << 7) | e.bytes[1]) - 8192) / 8191.0f, -1.0f, 1.0f);
        break;
    case 0xb0:
        if (e.bytes[1] == 64)
        {
            sustainDown = e.bytes[2] >= 64;
            if (!sustainDown)
                for (int n = 0; n < 128; ++n)
                    if (sustained[static_cast<std::size_t>(n)])
                    {
                        sustained[static_cast<std::size_t>(n)] = false;
                        noteOff(n);
                    }
        }
        else if (e.bytes[1] == 120)
            reset();
        else if (e.bytes[1] == 123)
            releaseAllNotes();
        break;
    default:
        break;
    }
}

// --- Rendering -------------------------------------------------------------------------------------------

void SamplerDevice::process(DeviceBuffers& io, const DeviceTimeInfo&) noexcept
{
    const int n = io.numSamples;
    float* left = io.channels[0];
    float* right = io.channels[io.numChannels > 1 ? 1 : 0];
    std::fill_n(left, n, 0.0f);
    if (right != left)
        std::fill_n(right, n, 0.0f);

    // A new sample: fade out what is playing from the old one, then switch.
    const auto* latest = sources.latest();
    if (latest != active)
    {
        if (active == nullptr || activeVoiceCount() == 0)
        {
            active = latest;
            sources.settle(active);
            switching = false;
        }
        else if (!switching)
        {
            switching = true;
            for (auto& v : voices)
                if (v.active && !v.stealing)
                {
                    v.stealing = true;
                    v.killStep = static_cast<float>(1.0 / (stealFadeSeconds * sampleRate()));
                }
        }
    }

    int pos = 0;
    for (const auto& e : *io.midiIn)
    {
        const int at = std::clamp(e.sampleOffset, 0, n);
        if (at > pos)
        {
            render(left + pos, right + pos, at - pos);
            pos = at;
        }
        handleEvent(e);
    }
    if (pos < n)
        render(left + pos, right + pos, n - pos);

    if (switching && activeVoiceCount() == 0)
    {
        active = latest;
        sources.settle(active);
        switching = false;
    }

    float l = 0.0f;
    float r = 0.0f;
    panGains(value(P::outPan), l, r);
    const float g = dbToGain(value(P::outVolume)) * 1.41421356f;
    const float targetL = g * l;
    const float targetR = g * r;
    for (int i = 0; i < n; ++i)
    {
        outGainLeft += std::clamp(targetL - outGainLeft, -gainStep, gainStep);
        outGainRight += std::clamp(targetR - outGainRight, -gainStep, gainStep);
        left[i] *= outGainLeft;
        if (right != left)
            right[i] *= outGainRight;
    }
}

void SamplerDevice::render(float* left, float* right, int numSamples) noexcept
{
    if (active == nullptr || active->object->sample == nullptr)
        return;
    for (auto& v : voices)
        if (v.active)
            renderVoice(v, left, right, numSamples);
}

void SamplerDevice::renderVoice(Voice& v, float* left, float* right, int numSamples) noexcept
{
    const auto& sample = *active->object->sample;
    const float* data[2] = {sample.channel(0), sample.channel(1)};
    const int channels = sample.numChannels;
    const bool draft = juce::roundToInt(value(P::quality)) == static_cast<int>(Quality::Draft);
    const auto& bank = dsp::SincKernelBank::get();
    const double sr = sampleRate();
    const float velocityGain = (1.0f - value(P::ampVelocity)) + value(P::ampVelocity) * v.velocity * v.velocity;
    const bool filterOn = value(P::filterOn) >= 0.5f;
    const int filterType = juce::roundToInt(value(P::filterType));
    dsp::SvfMode filterMode = dsp::SvfMode::LowPass;
    if (filterType == 2 || filterType == 3)
        filterMode = dsp::SvfMode::HighPass;
    else if (filterType == 4)
        filterMode = dsp::SvfMode::BandPass;
    else if (filterType == 5)
        filterMode = dsp::SvfMode::Notch;
    const bool steep = filterType == 1 || filterType == 3;
    const auto fadeInSamples = static_cast<double>(value(P::fadeIn)) * 0.001 * sr;
    const auto fadeOutSamples = static_cast<double>(value(P::fadeOut)) * 0.001 * sr;
    const double declick = declickSeconds * sr;
    const double crossfadeFrames =
        std::min(static_cast<double>(value(P::loopCrossfade)) * 0.001 * sample.sampleRate,
                 std::min(v.loopEnd - v.loopStart, v.loopStart));

    for (int start = 0; start < numSamples && v.active; start += controlBlock)
    {
        const int m = std::min(controlBlock, numSamples - start);
        const double rateTarget = rateFor(v);
        if (rateTarget <= 0.0)
        {
            v.active = false; // a note the tuning mutes
            return;
        }
        const double rateStep = (rateTarget - v.rate) / m;
        const auto& kernel = bank.levelFor(std::max(v.rate, rateTarget));
        float block[2][controlBlock];

        // Most blocks need neither fades nor the loop crossfade nor the region's end: those take a
        // plain loop. The interpolator is chosen outside the loop.
        const double reach = static_cast<double>(m) * std::max(v.rate, rateTarget);
        const double remainingAtEnd = (v.regionEnd - (v.position + reach)) / std::max(rateTarget, 1e-9);
        const bool fadingIn = static_cast<double>(v.played) < (v.oneShot ? std::max(fadeInSamples, declick) : declick) &&
                              (v.regionStart > 0.0 || (v.oneShot && fadeInSamples > 0.0));
        const bool nearEnd = !v.loop && remainingAtEnd < std::max(fadeOutSamples, declick) + 1.0;
        const bool crossfading = v.loop && crossfadeFrames > 0.0 && v.position + reach >= v.loopEnd - crossfadeFrames;
        const bool plain = !fadingIn && !nearEnd && !crossfading;

        const auto run = [&](auto&& read) {
            if (plain)
            {
                double x = v.position;
                double rate = v.rate;
                const double loopLength = v.loopEnd - v.loopStart;
                for (int i = 0; i < m; ++i)
                {
                    rate += rateStep;
                    float out[2];
                    read(x, out);
                    block[0][i] = out[0];
                    block[1][i] = out[1];
                    x += rate;
                    if (v.loop && x >= v.loopEnd)
                        x -= loopLength;
                }
                v.position = x;
                v.rate = rate;
                v.played += m;
                return;
            }
            for (int i = 0; i < m; ++i)
            {
                v.rate += rateStep;
                double x = v.position;
                if (x >= v.regionEnd)
                {
                    // The end of the region: the voice is done (the declick took it to silence).
                    for (int j = i; j < m; ++j)
                        block[0][j] = block[1][j] = 0.0f;
                    v.active = false;
                    break;
                }
                float out[2];
                read(x, out);
                if (v.loop && crossfadeFrames > 0.0 && x >= v.loopEnd - crossfadeFrames)
                {
                    // Crossfade the loop's end into the audio just before its start (equal power).
                    const double fade = (x - (v.loopEnd - crossfadeFrames)) / crossfadeFrames;
                    float other[2];
                    read(x - (v.loopEnd - v.loopStart), other);
                    const auto fadeOut = static_cast<float>(std::cos(fade * 1.5707963267948966));
                    const auto fadeIn = static_cast<float>(std::sin(fade * 1.5707963267948966));
                    out[0] = out[0] * fadeOut + other[0] * fadeIn;
                    out[1] = out[1] * fadeOut + other[1] * fadeIn;
                }

                // Fades: one-shot fade in and out, and a 1 ms declick wherever the region cuts the audio.
                float gain = 1.0f;
                const double elapsed = static_cast<double>(v.played);
                const double remaining = v.loop ? 1e30 : (v.regionEnd - x) / std::max(v.rate, 1e-9);
                if (v.oneShot && fadeInSamples > 0.0 && elapsed < fadeInSamples)
                    gain *= static_cast<float>(elapsed / fadeInSamples);
                if (v.oneShot && fadeOutSamples > 0.0 && remaining < fadeOutSamples)
                    gain *= static_cast<float>(remaining / fadeOutSamples);
                if (v.regionStart > 0.0 && elapsed < declick)
                    gain *= static_cast<float>(elapsed / declick);
                if (v.regionEnd < static_cast<double>(sample.length) && remaining < declick)
                    gain *= static_cast<float>(std::max(0.0, remaining) / declick);
                block[0][i] = out[0] * gain;
                block[1][i] = out[1] * gain;

                x += v.rate;
                if (v.loop && x >= v.loopEnd)
                    x -= v.loopEnd - v.loopStart;
                v.position = x;
                ++v.played;
            }
        };
        const auto split = [](double at, long long& index, float& frac) {
            index = static_cast<long long>(at);
            frac = static_cast<float>(at - static_cast<double>(index));
        };
        if (draft)
            run([&](double at, float* out) {
                long long index = 0;
                float frac = 0.0f;
                split(at, index, frac);
                dsp::hermiteInterpolate(data, channels, index, frac, out);
            });
        else
            run([&](double at, float* out) {
                long long index = 0;
                float frac = 0.0f;
                split(at, index, frac);
                dsp::sincInterpolate(kernel, data, channels, index, frac, out);
            });
        const int rendered = m; // a voice that finished left zeros in the rest of the block

        if (filterOn)
        {
            const float cutoff = std::clamp(value(P::filterCutoff), 20.0f, static_cast<float>(0.45 * sr));
            for (int ch = 0; ch < 2; ++ch)
            {
                v.svf[ch].setCoefficients(dsp::SvfCoefficients::prewarp(cutoff, sr), value(P::filterResonance), steep);
                v.svf[ch].processBlock(block[ch], rendered, 1.0f, filterMode);
            }
        }

        float env[controlBlock];
        v.amp.renderBlock(env, rendered);
        for (int i = 0; i < rendered; ++i)
        {
            float g = env[i] * velocityGain;
            if (v.stealing)
            {
                v.killGain = std::max(0.0f, v.killGain - v.killStep);
                g *= v.killGain;
            }
            left[start + i] += block[0][i] * g * v.panL * 1.41421356f;
            right[start + i] += block[1][i] * g * v.panR * 1.41421356f;
        }
        if (!v.amp.isActive() || (v.stealing && v.killGain <= 0.0f))
            v.active = false;
    }
}

} // namespace lattice
