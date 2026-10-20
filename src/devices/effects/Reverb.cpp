#include "devices/effects/Reverb.h"

#include "devices/dsp/Simd.h"
#include "devices/dsp/SyncRates.h"

#include <algorithm>
#include <cmath>

namespace lattice
{
namespace
{
using P = ReverbDevice::Param;
static_assert(ReverbDevice::maxLines == dsp::reverbTailMaxLines);

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

void ReverbDevice::prepare(double sr, int maxBlock)
{
    // Sized for the largest room at the tail's highest rate (Ultra: twice the sample rate).
    const double maxTailRate = 2.0 * sr;
    // Each line's memory: a power of two that holds its longest delay (and the four points of a
    // read), plus the guard; one allocation for all of them, each start 64-byte aligned.
    const auto& base = baseLengths();
    std::int32_t offset = 0;
    for (int i = 0; i < maxLines; ++i)
    {
        const double longest = base[static_cast<std::size_t>(i)] / 48000.0 * maxScale * maxTailRate;
        const int maxDelay = static_cast<int>(longest + maxModulation * maxTailRate) + 16;
        std::int32_t size = 4;
        while (size < maxDelay + 4)
            size <<= 1;
        tail.start[i] = offset;
        tail.mask[i] = size - 1;
        lines[static_cast<std::size_t>(i)].capacity = static_cast<double>(size - 4);
        offset += (size + dsp::reverbTailGuard + 15) & ~15;
    }
    tailMemory.assign(static_cast<std::size_t>(offset), 0.0f);
    tail.memory = tailMemory.data();
    tailKernel = dsp::reverbTailKernelFor(dsp::simdLevel());
    for (int d = 0; d < numDiffusers; ++d)
    {
        auto& diffuser = diffusers[static_cast<std::size_t>(d)];
        diffuser.length = std::max(1, static_cast<int>(diffuserSeconds[d] * sr));
        diffuser.delay.prepare(diffuser.length + 4);
    }
    // These are read a block at a time, after the block is written: room for one more block.
    predelayLine.prepare(static_cast<int>(maxPredelay * sr) + maxBlock + 4);
    earlyLine.prepare(static_cast<int>(0.2 * sr * maxScale) + maxBlock + 4);
    upsampler = std::make_unique<dsp::FirHalfBand>(47, 90.0);
    downsamplerL = std::make_unique<dsp::FirHalfBand>(47, 90.0);
    downsamplerR = std::make_unique<dsp::FirHalfBand>(47, 90.0);
    upsampler->prepareBlocks(maxBlock);
    downsamplerL->prepareBlocks(maxBlock);
    downsamplerR->prepareBlocks(maxBlock);
    // Per block: the tail's input, early reflections, and the tail's output (at up to 2x).
    for (auto* v : {&delayed, &scratch, &tailIn, &earlyOutL, &earlyOutR, &lateOutL, &lateOutR})
        v->assign(static_cast<std::size_t>(maxBlock), 0.0f);
    for (auto* v : {&groupL, &groupR})
        v->assign(static_cast<std::size_t>(earlyGroups * maxBlock), 0.0f);
    for (auto* v : {&tailIn2x, &tailOut2xL, &tailOut2xR})
        v->assign(static_cast<std::size_t>(2 * maxBlock), 0.0f);
    lastSize = -1.0f;
    lastShape = -1;
    lastLowCut = lastHighCut = -1.0f;
    reset();
    configure(true);
}

void ReverbDevice::clearTail() noexcept
{
    std::fill(tailMemory.begin(), tailMemory.end(), 0.0f);
    std::fill(std::begin(tail.low), std::end(tail.low), 0.0f);
    std::fill(std::begin(tail.high), std::end(tail.high), 0.0f);
}

void ReverbDevice::reset() noexcept
{
    clearTail();
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
        clearTail(); // a different rate and line count: start the tail afresh
    }
    numLines = quality == Quality::Eco ? 8 : (quality == Quality::Ultra ? 32 : 16);
    tail.count = numLines;
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
            modulationCountdown = 0; // read positions from these at the next sample
        }
        // Each line's own slow, irregular modulation.
        random = random * 1664525u + 1013904223u;
        if (snap)
        {
            l.modRate = 0.1 + 0.8 * static_cast<double>(random >> 8) / 16777216.0;
            const double phase = 6.283185307179586 * static_cast<double>((random * 69069u) >> 8) / 16777216.0;
            l.modSin = std::sin(phase);
            l.modCos = std::cos(phase);
            const double turn = 6.283185307179586 * l.modRate * modulationBlock / tailRate;
            l.turnSin = std::sin(turn);
            l.turnCos = std::cos(turn);
        }
    }

    // Per-band gains per trip round each line, from its current length: again only when those or
    // the decay settings have moved.
    const bool frozen = value(P::freeze) >= 0.5f;
    const double averageModulation = static_cast<double>(value(P::modulation)) * maxModulation * tailRate;
    bool gliding = false;
    for (int i = 0; i < numLines; ++i)
        gliding = gliding || lines[static_cast<std::size_t>(i)].length != lines[static_cast<std::size_t>(i)].targetLength;
    const std::array<double, 6> inputs{bandDecay(0), bandDecay(1), bandDecay(2), averageModulation, frozen ? 1.0 : 0.0,
                                       static_cast<double>(numLines)};
    const bool update = snap || gliding || inputs != gainInputs;
    for (int i = 0; update && i < numLines; ++i)
    {
        // A trip round the line takes its length plus the modulation's average.
        const double length = lines[static_cast<std::size_t>(i)].length + averageModulation;
        const auto gainFor = [&](double rt60) {
            return frozen ? 1.0f : static_cast<float>(std::pow(10.0, -3.0 * length / (std::max(0.05, rt60) * tailRate)));
        };
        tail.lowGain[i] = gainFor(bandDecay(0));
        tail.midGain[i] = gainFor(bandDecay(1));
        tail.highGain[i] = gainFor(bandDecay(2));
    }
    gainInputs = inputs;
    tail.lowCoef = static_cast<float>(1.0 - std::exp(-2.0 * 3.141592653589793 * value(P::lowCrossover) / tailRate));
    tail.highCoef = static_cast<float>(1.0 - std::exp(-2.0 * 3.141592653589793 * value(P::highCrossover) / tailRate));

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

void ReverbDevice::updateModulation() noexcept
{
    // The modulation and the length glides move at control rate; each line's read position moves
    // in a straight line between updates (dsp::ReverbTailState).
    const double depth = value(P::freeze) >= 0.5f ? 0.0 : static_cast<double>(value(P::modulation)) * maxModulation * tailRate;
    const double glide = 1.0 - std::exp(-static_cast<double>(modulationBlock) / (0.3 * tailRate));
    for (int i = 0; i < numLines; ++i)
    {
        auto& l = lines[static_cast<std::size_t>(i)];
        if (l.length != l.targetLength)
        {
            l.length += (l.targetLength - l.length) * glide;
            if (std::abs(l.targetLength - l.length) < 1.0e-3) // arrived (a glide never quite does)
                l.length = l.targetLength;
        }
        // The modulation's phase turns by one step, kept on the unit circle (rounding would
        // otherwise change its size over hours).
        const double s = l.modSin * l.turnCos + l.modCos * l.turnSin;
        const double c = l.modCos * l.turnCos - l.modSin * l.turnSin;
        const double size = 1.5 - 0.5 * (s * s + c * c);
        l.modSin = s * size;
        l.modCos = c * size;
        // Read positions are delays plus one (the write comes after the read), from 2 (a delay
        // of 1 keeps the interpolator causal) to what the memory holds.
        const double next = std::clamp(l.length + depth * (1.0 + l.modSin), 2.0,
                                       l.capacity + 1.0);
        // Delays from l.modOffset - 1 to next - 1 over the run: whole + x, with x >= 0 throughout.
        const double from = l.modOffset - 1.0;
        const double whole = std::floor(std::min(from, next - 1.0));
        tail.whole[i] = static_cast<std::int32_t>(whole);
        tail.x[i] = static_cast<float>(from - whole);
        tail.step[i] = static_cast<float>((next - l.modOffset) / modulationBlock);
        l.modOffset = next;
    }
}

void ReverbDevice::runTail(const float* in, int n, float* outL, float* outR) noexcept
{
    for (int done = 0; done < n;)
    {
        if (modulationCountdown <= 0)
        {
            updateModulation();
            modulationCountdown = modulationBlock;
        }
        const int run = std::min(modulationCountdown, n - done);
        tailKernel(tail, in + done, run, outL + done, outR + done);
        modulationCountdown -= run;
        done += run;
    }
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
    tail.outputNorm = 1.0f / std::sqrt(static_cast<float>(numLines / 2));
    // A whole number of samples: nothing to interpolate, and no dulling of the input.
    const int predelayTap = static_cast<int>(std::lround(predelaySamples));

    // 1. The input: pre-delay, early reflections, input diffusion. Each stage takes the whole
    // block (each delay line is written for the block, then read along contiguous memory), which
    // gives the same samples as going one sample at a time.
    for (int i = 0; i < n; ++i)
        scratch[static_cast<std::size_t>(i)] = frozen ? 0.0f : 0.5f * (l[i] + r[i]);
    predelayLine.pushBlock(scratch.data(), n);
    predelayLine.forEachRun(predelayTap, n, [&](const float* x, int count, int at) {
        std::copy(x, x + count, delayed.begin() + at);
    });

    // Early reflections from the pre-delayed input. Taps are summed per distance group, and each
    // group darkened by one filter pair: far reflections duller than near ones, for 8 filters
    // instead of one per tap.
    earlyLine.pushBlock(delayed.data(), n);
    if (early > 0.0f)
    {
        constexpr int perGroup = numTaps / earlyGroups;
        std::fill(groupL.begin(), groupL.begin() + earlyGroups * n, 0.0f);
        std::fill(groupR.begin(), groupR.begin() + earlyGroups * n, 0.0f);
        for (int k = 0; k < numTaps; ++k)
        {
            const auto& tap = taps[static_cast<std::size_t>(k)];
            float* sumL = groupL.data() + (k / perGroup) * n;
            float* sumR = groupR.data() + (k / perGroup) * n;
            earlyLine.forEachRun(tap.delay, n, [&](const float* x, int count, int at) {
                for (int j = 0; j < count; ++j)
                {
                    sumL[at + j] += tap.gainL * x[j];
                    sumR[at + j] += tap.gainR * x[j];
                }
            });
        }
        float c[earlyGroups];
        for (int group = 0; group < earlyGroups; ++group)
            c[group] = 1.0f - taps[static_cast<std::size_t>(group * perGroup + perGroup / 2)].damping;
        for (int i = 0; i < n; ++i)
        {
            float earlyL = 0.0f;
            float earlyR = 0.0f;
            for (int group = 0; group < earlyGroups; ++group)
            {
                const auto at = static_cast<std::size_t>(group * n + i);
                tapDampState[2 * group] += c[group] * (groupL[at] - tapDampState[2 * group]);
                tapDampState[2 * group + 1] += c[group] * (groupR[at] - tapDampState[2 * group + 1]);
                earlyL += tapDampState[2 * group];
                earlyR += tapDampState[2 * group + 1];
            }
            earlyOutL[static_cast<std::size_t>(i)] = earlyL;
            earlyOutR[static_cast<std::size_t>(i)] = earlyR;
        }
    }
    else
    {
        std::fill(earlyOutL.begin(), earlyOutL.begin() + n, 0.0f);
        std::fill(earlyOutR.begin(), earlyOutR.begin() + n, 0.0f);
    }

    // Input diffusion: all-passes in series, each over the block in runs no longer than its
    // delay (so a run reads only what was written before it).
    std::copy(delayed.begin(), delayed.begin() + n, tailIn.begin());
    for (auto& d : diffusers)
    {
        for (int done = 0; done < n;)
        {
            const int run = std::min(n - done, d.length);
            float* x = tailIn.data() + done;
            float* w = scratch.data();
            d.delay.forEachRun(d.length - run, run, [&](const float* old, int count, int at) {
                for (int j = at; j < at + count; ++j)
                {
                    w[j] = x[j] + g * old[j - at];
                    x[j] = -g * w[j] + old[j - at];
                }
            });
            d.delay.pushBlock(w, run);
            done += run;
        }
    }
    for (int i = 0; i < n; ++i)
        tailIn[static_cast<std::size_t>(i)] *= inputScale;

    // 2. The tail: at the sample rate, or (Ultra) at twice it, with the rate changes done in blocks.
    if (ultra)
    {
        upsampler->upsampleBlock(tailIn.data(), n, tailIn2x.data());
        runTail(tailIn2x.data(), 2 * n, tailOut2xL.data(), tailOut2xR.data());
        downsamplerL->downsampleBlock(tailOut2xL.data(), n, lateOutL.data());
        downsamplerR->downsampleBlock(tailOut2xR.data(), n, lateOutR.data());
    }
    else
    {
        runTail(tailIn.data(), n, lateOutL.data(), lateOutR.data());
    }

    // 3. Early and late together, width, the wet signal's filters, and the mix.
    for (int i = 0; i < n; ++i)
    {
        const float dryL = l[i];
        const float dryR = r[i];
        const float earlyL = earlyOutL[static_cast<std::size_t>(i)];
        const float earlyR = earlyOutR[static_cast<std::size_t>(i)];
        const float lateL = lateOutL[static_cast<std::size_t>(i)];
        const float lateR = lateOutR[static_cast<std::size_t>(i)];
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
