#include "devices/effects/Reverb.h"

#include "devices/dsp/SyncRates.h"

#include <algorithm>
#include <cmath>

namespace lattice
{
namespace
{
using P = ReverbDevice::Param;

constexpr double minScale = 0.25; // line lengths relative to the base set, at size 0 and 1
constexpr double maxScale = 2.0;
constexpr double shortestLine = 0.023; // seconds, base set
constexpr double longestLine = 0.109;
constexpr double maxModulation = 0.0002; // seconds of delay modulation at 100 %
constexpr double maxPredelay = 1.0;      // seconds

std::vector<ParameterSpec> makeSpecs()
{
    std::vector<ParameterSpec> s(static_cast<std::size_t>(P::numParams));
    const auto set = [&](int index, ParameterSpec spec, const char* group) {
        spec.group = group;
        s[static_cast<std::size_t>(index)] = std::move(spec);
    };
    const auto pct = [](float v) { return juce::String(juce::roundToInt(v * 100.0f)) + " %"; };
    const auto times = [](float v) { return juce::String(v, 2) + " x"; };
    set(P::size, ParameterSpec::continuous("reverb.size", "Size", 0.0f, 1.0f, 0.6f, ParamUnit::Percent).withFormatter(pct), "Reverb");
    set(P::decay, ParameterSpec::continuous("reverb.decay", "Decay", 0.2f, 60.0f, 2.5f, ParamUnit::Seconds).withCentre(3.0f), "Reverb");
    set(P::lowDecay, ParameterSpec::continuous("reverb.lowDecay", "Low Decay", 0.25f, 4.0f, 1.2f).withCentre(1.0f).withFormatter(times), "Decay");
    set(P::highDecay, ParameterSpec::continuous("reverb.highDecay", "High Decay", 0.1f, 2.0f, 0.5f).withCentre(1.0f).withFormatter(times), "Decay");
    set(P::lowCrossover, ParameterSpec::continuous("reverb.lowCrossover", "Low X-Over", 50.0f, 1000.0f, 300.0f, ParamUnit::Hz).withCentre(250.0f), "Decay");
    set(P::highCrossover, ParameterSpec::continuous("reverb.highCrossover", "High X-Over", 1000.0f, 16000.0f, 4000.0f, ParamUnit::Hz).withCentre(4000.0f), "Decay");
    set(P::predelay, ParameterSpec::continuous("reverb.predelay", "Pre-Delay", 0.0f, 500.0f, 10.0f, ParamUnit::Milliseconds).withCentre(50.0f), "Reverb");
    set(P::predelaySync, ParameterSpec::toggle("reverb.predelaySync", "Pre-Delay Sync", false), "Reverb");
    set(P::predelayBeats, ParameterSpec::choice("reverb.predelayBeats", "Synced Pre-Delay", dsp::SyncRates::names(), dsp::SyncRates::indexOf("1/32")), "Reverb");
    set(P::diffusion, ParameterSpec::continuous("reverb.diffusion", "Diffusion", 0.0f, 1.0f, 0.7f, ParamUnit::Percent).withFormatter(pct), "Character");
    set(P::shape, ParameterSpec::choice("reverb.shape", "Shape", {"Room", "Hall", "Chamber", "Plate"}, 1), "Character");
    set(P::earlyLevel, ParameterSpec::continuous("reverb.early", "Early", -70.0f, 6.0f, -6.0f, ParamUnit::Decibels).withCentre(-12.0f), "Character");
    set(P::lateLevel, ParameterSpec::continuous("reverb.late", "Late", -70.0f, 6.0f, 0.0f, ParamUnit::Decibels).withCentre(-12.0f), "Character");
    set(P::modulation, ParameterSpec::continuous("reverb.modulation", "Modulation", 0.0f, 1.0f, 0.3f, ParamUnit::Percent).withFormatter(pct), "Character");
    set(P::width, ParameterSpec::continuous("reverb.width", "Width", 0.0f, 1.5f, 1.0f, ParamUnit::Percent).withFormatter(pct), "Output");
    set(P::lowCut, ParameterSpec::continuous("reverb.lowCut", "Low Cut", 20.0f, 2000.0f, 60.0f, ParamUnit::Hz).withCentre(200.0f), "Output");
    set(P::highCut, ParameterSpec::continuous("reverb.highCut", "High Cut", 1000.0f, 20000.0f, 16000.0f, ParamUnit::Hz).withCentre(6000.0f), "Output");
    set(P::freeze, ParameterSpec::toggle("reverb.freeze", "Freeze", false), "Output");
    set(P::quality, ParameterSpec::choice("reverb.quality", "Quality", {"Eco", "High", "Ultra"}, 1), "Output");
    set(P::mix, ParameterSpec::continuous("reverb.mix", "Dry/Wet", 0.0f, 1.0f, 0.3f, ParamUnit::Percent).withFormatter(pct), "Output");
    return s;
}

bool isPrime(int n)
{
    if (n < 2)
        return false;
    for (int d = 2; d * d <= n; ++d)
        if (n % d == 0)
            return false;
    return true;
}

/** The base line lengths (at 48 kHz, size scale 1): primes spread geometrically. */
const std::array<int, ReverbDevice::maxLines>& baseLengths()
{
    static const std::array<int, ReverbDevice::maxLines> lengths = [] {
        std::array<int, ReverbDevice::maxLines> out{};
        for (int i = 0; i < ReverbDevice::maxLines; ++i)
        {
            const double t = shortestLine * std::pow(longestLine / shortestLine, i / (ReverbDevice::maxLines - 1.0));
            int n = static_cast<int>(t * 48000.0);
            while (!isPrime(n))
                ++n;
            out[static_cast<std::size_t>(i)] = n;
        }
        // Interleave long and short, so any subset (Eco takes every other) spans the range.
        std::array<int, ReverbDevice::maxLines> mixed{};
        for (int i = 0; i < ReverbDevice::maxLines / 2; ++i)
        {
            mixed[static_cast<std::size_t>(2 * i)] = out[static_cast<std::size_t>(i)];
            mixed[static_cast<std::size_t>(2 * i + 1)] = out[static_cast<std::size_t>(ReverbDevice::maxLines - 1 - i)];
        }
        return mixed;
    }();
    return lengths;
}

constexpr double diffuserSeconds[ReverbDevice::numDiffusers] = {0.00477, 0.00359, 0.01273, 0.00931, 0.00713, 0.00531};

float dbToGain(float db)
{
    return db <= -69.95f ? 0.0f : std::pow(10.0f, db / 20.0f);
}
} // namespace

ReverbDevice::ReverbDevice() : StudioDevice(typeId, "Reverb", DeviceKind::AudioEffect, makeSpecs())
{
}

double ReverbDevice::bandDecay(int band) const noexcept
{
    const double mid = value(P::decay);
    return band == 0 ? mid * value(P::lowDecay) : (band == 2 ? mid * value(P::highDecay) : mid);
}

void ReverbDevice::prepare(double sr, int)
{
    // Sized for the largest room at the tail's highest rate (Ultra: twice the sample rate).
    const double maxTailRate = 2.0 * sr;
    const auto& base = baseLengths();
    for (int i = 0; i < maxLines; ++i)
    {
        const double longest = base[static_cast<std::size_t>(i)] / 48000.0 * maxScale * maxTailRate;
        lines[static_cast<std::size_t>(i)].delay.prepare(static_cast<int>(longest + maxModulation * maxTailRate) + 16);
    }
    for (int d = 0; d < numDiffusers; ++d)
    {
        auto& diffuser = diffusers[static_cast<std::size_t>(d)];
        diffuser.length = std::max(1, static_cast<int>(diffuserSeconds[d] * sr));
        diffuser.delay.prepare(diffuser.length + 4);
    }
    predelayLine.prepare(static_cast<int>(maxPredelay * sr) + 4);
    earlyLine.prepare(static_cast<int>(0.2 * sr * maxScale) + 4);
    upsampler = std::make_unique<dsp::FirHalfBand>(47, 90.0);
    downsamplerL = std::make_unique<dsp::FirHalfBand>(47, 90.0);
    downsamplerR = std::make_unique<dsp::FirHalfBand>(47, 90.0);
    lastSize = -1.0f;
    lastShape = -1;
    lastLowCut = lastHighCut = -1.0f;
    reset();
    configure(true);
}

void ReverbDevice::reset() noexcept
{
    for (auto& l : lines)
        l.delay.reset();
    std::fill(std::begin(lowState), std::end(lowState), 0.0f);
    std::fill(std::begin(highState), std::end(highState), 0.0f);
    for (auto& d : diffusers)
        d.delay.reset();
    predelayLine.reset();
    earlyLine.reset();
    for (auto& s : tapDampState)
        s = 0.0f;
    for (int ch = 0; ch < 2; ++ch)
    {
        lowCutFilter[ch].reset();
        highCutFilter[ch].reset();
    }
    if (upsampler != nullptr)
    {
        upsampler->reset();
        downsamplerL->reset();
        downsamplerR->reset();
    }
}

void ReverbDevice::configure(bool snap) noexcept
{
    const auto quality = static_cast<Quality>(juce::roundToInt(value(P::quality)));
    if (quality != currentQuality)
    {
        currentQuality = quality;
        snap = true;
        for (auto& l : lines) // a different rate and line count: start the tail afresh
            l.delay.reset();
        std::fill(std::begin(lowState), std::end(lowState), 0.0f);
        std::fill(std::begin(highState), std::end(highState), 0.0f);
    }
    numLines = quality == Quality::Eco ? 8 : (quality == Quality::Ultra ? 32 : 16);
    tailRate = quality == Quality::Ultra ? 2.0 * sampleRate() : sampleRate();

    const double scale = minScale + (maxScale - minScale) * static_cast<double>(value(P::size)) * value(P::size);
    const auto& base = baseLengths();
    for (int i = 0; i < numLines; ++i)
    {
        auto& l = lines[static_cast<std::size_t>(i)];
        const int index = quality == Quality::Eco ? i * 4 : (quality == Quality::High ? i * 2 : i);
        l.targetLength = base[static_cast<std::size_t>(index)] / 48000.0 * scale * tailRate;
        if (value(P::freeze) >= 0.5f)
            l.targetLength = std::round(l.targetLength); // whole samples: the frozen loop loses nothing
        if (snap)
        {
            l.length = l.targetLength;
            l.modOffset = l.length;
            l.modStep = 0.0;
        }
        // Each line's own slow, irregular modulation.
        random = random * 1664525u + 1013904223u;
        if (snap)
        {
            l.modRate = 0.1 + 0.8 * static_cast<double>(random >> 8) / 16777216.0;
            l.modPhase = static_cast<double>((random * 69069u) >> 8) / 16777216.0;
        }
    }

    // Per-band gains per trip round each line, from its current length.
    const bool frozen = value(P::freeze) >= 0.5f;
    const double averageModulation = static_cast<double>(value(P::modulation)) * maxModulation * tailRate;
    for (int i = 0; i < numLines; ++i)
    {
        // A trip round the line takes its length plus the modulation's average.
        const double length = lines[static_cast<std::size_t>(i)].length + averageModulation;
        const auto gainFor = [&](double rt60) {
            return frozen ? 1.0f : static_cast<float>(std::pow(10.0, -3.0 * length / (std::max(0.05, rt60) * tailRate)));
        };
        lowGain[static_cast<std::size_t>(i)] = gainFor(bandDecay(0));
        lineGain[static_cast<std::size_t>(i)] = gainFor(bandDecay(1));
        highGain[static_cast<std::size_t>(i)] = gainFor(bandDecay(2));
    }
    lowCoef = static_cast<float>(1.0 - std::exp(-2.0 * 3.141592653589793 * value(P::lowCrossover) / tailRate));
    highCoef = static_cast<float>(1.0 - std::exp(-2.0 * 3.141592653589793 * value(P::highCrossover) / tailRate));

    // Early reflections: taps from the room's shape and size (deterministic per shape).
    const int shapeIndex = juce::roundToInt(value(P::shape));
    if (snap || shapeIndex != lastShape || value(P::size) != lastSize)
    {
        lastShape = shapeIndex;
        lastSize = value(P::size);
        const auto shape = static_cast<RoomShape>(shapeIndex);
        const double spread = shape == RoomShape::Hall ? 0.08 : shape == RoomShape::Room ? 0.03 : shape == RoomShape::Chamber ? 0.05 : 0.015;
        std::uint32_t seed = 0x1234u + static_cast<std::uint32_t>(shapeIndex) * 7919u;
        for (int k = 0; k < numTaps; ++k)
        {
            seed = seed * 1664525u + 1013904223u;
            const double u = static_cast<double>(seed >> 8) / 16777216.0;
            // Denser towards the end, as reflections multiply.
            const double t = 0.002 + spread * scale * std::sqrt((k + u) / numTaps);
            seed = seed * 1664525u + 1013904223u;
            const float pan = static_cast<float>(seed >> 8) / 16777216.0f * 2.0f - 1.0f;
            const auto distance = static_cast<float>(t / (spread * scale + 0.002));
            const float g = 0.9f / (1.0f + 6.0f * distance) * (shape == RoomShape::Plate ? 1.3f : 1.0f);
            auto& tap = taps[static_cast<std::size_t>(k)];
            tap.delay = std::max(1, static_cast<int>(t * sampleRate()));
            tap.gainL = g * std::cos((pan + 1.0f) * 0.785398f);
            tap.gainR = g * std::sin((pan + 1.0f) * 0.785398f);
            tap.damping = 0.9f - 0.7f * distance; // one-pole coefficient: far taps darker
        }
    }

    if (value(P::lowCut) != lastLowCut || value(P::highCut) != lastHighCut)
    {
        lastLowCut = value(P::lowCut);
        lastHighCut = value(P::highCut);
        const auto lc = dsp::BiquadCoefficients::design(dsp::BiquadType::HighPass, lastLowCut, 0.7071, 0.0, sampleRate());
        const auto hc = dsp::BiquadCoefficients::design(dsp::BiquadType::LowPass, lastHighCut, 0.7071, 0.0, sampleRate());
        for (int ch = 0; ch < 2; ++ch)
        {
            lowCutFilter[ch].set(lc);
            highCutFilter[ch].set(hc);
        }
    }
}

void ReverbDevice::tailTick(float in, float& outL, float& outR) noexcept
{
    const int n = numLines;
    if (--modulationCountdown <= 0)
    {
        // The modulation and the length glides move at control rate, interpolated in between.
        modulationCountdown = modulationBlock;
        const double depth = value(P::freeze) >= 0.5f ? 0.0 : static_cast<double>(value(P::modulation)) * maxModulation * tailRate;
        const double glide = 1.0 - std::exp(-static_cast<double>(modulationBlock) / (0.3 * tailRate));
        for (int i = 0; i < n; ++i)
        {
            auto& l = lines[static_cast<std::size_t>(i)];
            if (l.length != l.targetLength)
                l.length += (l.targetLength - l.length) * glide;
            l.modPhase += l.modRate * modulationBlock / tailRate;
            if (l.modPhase >= 1.0)
                l.modPhase -= 1.0;
            const double next = l.length + depth * (1.0 + std::sin(6.283185307179586 * l.modPhase));
            l.modStep = (next - l.modOffset) / modulationBlock;
        }
    }
    alignas(16) float raw[maxLines];
    for (int i = 0; i < n; ++i)
    {
        auto& l = lines[static_cast<std::size_t>(i)];
        l.modOffset += l.modStep;
        raw[i] = l.delay.read(static_cast<float>(l.modOffset - 1.0));
    }
    // Three bands per line (below the low crossover, between, above the high one), each with its
    // own gain per trip. Plain loops over arrays: the compiler vectorises them.
    alignas(16) float outputs[maxLines];
    for (int i = 0; i < n; ++i)
    {
        lowState[i] += lowCoef * (raw[i] - lowState[i]);
        const float rest = raw[i] - lowState[i];
        highState[i] += highCoef * (rest - highState[i]);
        outputs[i] = lowGain[static_cast<std::size_t>(i)] * lowState[i] + lineGain[static_cast<std::size_t>(i)] * highState[i] +
                     highGain[static_cast<std::size_t>(i)] * (rest - highState[i]);
    }
    float sum = 0.0f;
    for (int i = 0; i < n; ++i)
        sum += outputs[i];
    // Householder feedback: each line gets its own output minus 2/N of them all.
    const float k = 2.0f / static_cast<float>(n);
    float left = 0.0f;
    float right = 0.0f;
    for (int i = 0; i < n; ++i)
    {
        const float sign = (i & 2) != 0 ? -1.0f : 1.0f;
        lines[static_cast<std::size_t>(i)].delay.push(outputs[i] - k * sum + in * sign);
        if ((i & 1) == 0)
            left += sign * outputs[i];
        else
            right += sign * outputs[i];
    }
    const float norm = 1.0f / std::sqrt(static_cast<float>(n / 2));
    outL = left * norm;
    outR = right * norm;
}

void ReverbDevice::process(DeviceBuffers& io, const DeviceTimeInfo& time) noexcept
{
    configure(false);
    const int n = io.numSamples;
    const double sr = sampleRate();
    float* l = io.channels[0];
    float* r = io.channels[1];
    const bool frozen = value(P::freeze) >= 0.5f;
    const double predelaySamples = std::clamp(
        value(P::predelaySync) >= 0.5f ? dsp::SyncRates::beatsAt(juce::roundToInt(value(P::predelayBeats))) * time.samplesPerBeat
                                       : static_cast<double>(value(P::predelay)) * 0.001 * sr,
        1.0, maxPredelay * sr);
    const float g = 0.7f * value(P::diffusion);
    const float early = dbToGain(value(P::earlyLevel));
    const float late = dbToGain(value(P::lateLevel));
    const float width = value(P::width);
    const float targetMix = value(P::mix);
    const float mixStep = (targetMix - mixValue) / static_cast<float>(n);
    const bool ultra = currentQuality == Quality::Ultra;
    const float inputScale = 1.0f / std::sqrt(static_cast<float>(numLines));

    for (int i = 0; i < n; ++i)
    {
        const float dryL = l[i];
        const float dryR = r[i];
        const float mono = frozen ? 0.0f : 0.5f * (dryL + dryR);
        predelayLine.push(mono);
        // A whole number of samples: nothing to interpolate, and no dulling of the input.
        const float delayed = predelayLine.tap(static_cast<int>(std::lround(predelaySamples))); // pushed just now: tap 0

        // Early reflections from the pre-delayed input.
        earlyLine.push(delayed);
        float earlyL = 0.0f;
        float earlyR = 0.0f;
        if (early > 0.0f)
        {
            // Taps are summed per distance group, and each group darkened by one filter pair: far
            // reflections duller than near ones, for 8 filters instead of one per tap.
            constexpr int perGroup = numTaps / earlyGroups;
            for (int group = 0; group < earlyGroups; ++group)
            {
                float sumL = 0.0f;
                float sumR = 0.0f;
                for (int k = group * perGroup; k < (group + 1) * perGroup; ++k)
                {
                    const auto& tap = taps[static_cast<std::size_t>(k)];
                    const float x = earlyLine.tap(tap.delay);
                    sumL += tap.gainL * x;
                    sumR += tap.gainR * x;
                }
                const float c = 1.0f - taps[static_cast<std::size_t>(group * perGroup + perGroup / 2)].damping;
                tapDampState[2 * group] += c * (sumL - tapDampState[2 * group]);
                tapDampState[2 * group + 1] += c * (sumR - tapDampState[2 * group + 1]);
                earlyL += tapDampState[2 * group];
                earlyR += tapDampState[2 * group + 1];
            }
        }

        // Input diffusion: all-passes in series.
        float x = delayed;
        for (auto& d : diffusers)
        {
            const float w = x + g * d.delay.tap(d.length - 1);
            const float y = -g * w + d.delay.tap(d.length - 1);
            d.delay.push(w);
            x = y;
        }
        x *= inputScale;

        float lateL = 0.0f;
        float lateR = 0.0f;
        if (ultra)
        {
            float u0 = 0.0f;
            float u1 = 0.0f;
            upsampler->upsample(x, u0, u1);
            float a0 = 0.0f, a1 = 0.0f, b0 = 0.0f, b1 = 0.0f;
            tailTick(u0, a0, b0);
            tailTick(u1, a1, b1);
            lateL = downsamplerL->downsample(a0, a1);
            lateR = downsamplerR->downsample(b0, b1);
        }
        else
        {
            tailTick(x, lateL, lateR);
        }

        float wetL = early * earlyL + late * lateL;
        float wetR = early * earlyR + late * lateR;
        // Width (mid/side), then the wet signal's high and low cut.
        const float mid = 0.5f * (wetL + wetR);
        const float side = 0.5f * (wetL - wetR) * width;
        wetL = highCutFilter[0].process(lowCutFilter[0].process(mid + side));
        wetR = highCutFilter[1].process(lowCutFilter[1].process(mid - side));

        mixValue += mixStep;
        l[i] = dryL + mixValue * (wetL - dryL);
        r[i] = dryR + mixValue * (wetR - dryR);
    }
}

} // namespace lattice
