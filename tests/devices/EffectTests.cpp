// Audio effects (docs/04-m3-devices.md, "Tests" 6).

#include "devices/dsp/SyncRates.h"
#include "devices/effects/Compressor.h"
#include "devices/effects/Delay.h"
#include "devices/effects/EqEight.h"
#include "devices/effects/Limiter.h"
#include "devices/effects/Reverb.h"
#include "devices/effects/Saturator.h"
#include "devices/effects/Utility.h"
#include "support/DeviceHarness.h"
#include "support/Spectrum.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cmath>
#include <numeric>

using namespace lattice;
using namespace lattice::test;

namespace
{
constexpr double sr = DeviceHarness::sampleRate;
constexpr double pi = 3.14159265358979323846;

double toDb(double g)
{
    return 20.0 * std::log10(std::max(g, 1e-15));
}

double rms(const std::vector<float>& x, std::size_t from, std::size_t to)
{
    double s = 0.0;
    for (auto i = from; i < to; ++i)
        s += static_cast<double>(x[i]) * x[i];
    return std::sqrt(s / static_cast<double>(to - from));
}

/** A stereo input from a function of the sample. */
template <typename F>
void feed(DeviceHarness& h, F&& f)
{
    h.inputSignal = [f](std::int64_t i) {
        const float v = f(i);
        return std::pair<float, float>{v, v};
    };
}
} // namespace

TEST_CASE("EQ Eight: the measured response is the analytic one within 0.1 dB", "[devices][effects][eq][heavy]")
{
    EqEightDevice eq;
    // A busy setting: a 48 dB low cut, three bells, a shelf, a 12 dB high cut.
    const auto band = [&](int b, EqEightDevice::Shape shape, float hz, float gain, float q) {
        setParam(eq, ("band" + juce::String(b + 1) + ".on").toRawUTF8(), 1.0f);
        setParam(eq, ("band" + juce::String(b + 1) + ".shape").toRawUTF8(), static_cast<float>(shape));
        setParam(eq, ("band" + juce::String(b + 1) + ".frequency").toRawUTF8(), hz);
        setParam(eq, ("band" + juce::String(b + 1) + ".gain").toRawUTF8(), gain);
        setParam(eq, ("band" + juce::String(b + 1) + ".q").toRawUTF8(), q);
    };
    band(0, EqEightDevice::Shape::LowCut48, 40.0f, 0.0f, 0.71f);
    band(1, EqEightDevice::Shape::Bell, 180.0f, 6.0f, 1.4f);
    band(2, EqEightDevice::Shape::Bell, 1100.0f, -9.0f, 3.0f);
    band(3, EqEightDevice::Shape::Bell, 3500.0f, 4.0f, 0.7f);
    band(4, EqEightDevice::Shape::HighShelf, 9000.0f, -6.0f, 0.71f);
    band(5, EqEightDevice::Shape::HighCut12, 18000.0f, 0.0f, 0.71f);
    setParam(eq, "band7.on", 0.0f);
    setParam(eq, "band8.on", 0.0f);

    for (const double hz : {30.0, 60.0, 180.0, 700.0, 1100.0, 3500.0, 9000.0, 14000.0, 19000.0})
    {
        INFO(hz << " Hz");
        DeviceHarness h(eq);
        feed(h, [hz](std::int64_t i) { return static_cast<float>(0.25 * std::sin(2.0 * pi * hz * static_cast<double>(i) / sr)); });
        h.renderTo(48000 * 2);
        const double measured = toDb(rms(h.audioLeft, 48000, 96000) / (0.25 / std::sqrt(2.0)));
        CHECK(measured == Catch::Approx(toDb(std::abs(eq.response(hz)))).margin(0.1));
    }
}

TEST_CASE("Compressor: the static curve matches threshold, ratio and knee within 0.2 dB", "[devices][effects][dynamics]")
{
    for (const float knee : {0.0f, 6.0f, 12.0f})
    {
        CompressorDevice comp;
        setParam(comp, "comp.threshold", -20.0f);
        setParam(comp, "comp.ratio", 4.0f);
        setParam(comp, "comp.knee", knee);
        setParam(comp, "comp.attack", 1.0f);
        setParam(comp, "comp.release", 50.0f);
        for (const float levelDb : {-40.0f, -26.0f, -23.0f, -20.0f, -17.0f, -10.0f, 0.0f})
        {
            INFO("knee " << knee << " dB, input " << levelDb << " dB");
            DeviceHarness h(comp);
            const float level = std::pow(10.0f, levelDb / 20.0f);
            feed(h, [level](std::int64_t) { return level; }); // a steady level settles exactly
            h.renderTo(48000);
            const double outDb = toDb(std::abs(h.audioLeft.back()));
            // Curve from the device, and from the textbook formula.
            CHECK(outDb == Catch::Approx(comp.staticCurve(levelDb)).margin(0.2));
            double expected = levelDb;
            const double over = levelDb + 20.0;
            if (knee > 0.0f && 2.0 * std::abs(over) <= knee)
                expected = levelDb + (1.0 / 4.0 - 1.0) * std::pow(over + knee / 2.0, 2.0) / (2.0 * knee);
            else if (2.0 * over > knee)
                expected = -20.0 + over / 4.0;
            CHECK(outDb == Catch::Approx(expected).margin(0.2));
        }
    }
}

TEST_CASE("Delay: the time is exact to the sample at every synced value", "[devices][effects][delay]")
{
    for (int rate = 0; rate < static_cast<int>(dsp::SyncRates::rates.size()); ++rate)
    {
        const double beats = dsp::SyncRates::beatsAt(rate);
        const double samples = beats * sr * 60.0 / 120.0;
        if (samples > DelayDevice::maxSeconds * sr - 16)
            continue;
        INFO(dsp::SyncRates::rates[static_cast<std::size_t>(rate)].name);
        DelayDevice delay;
        setParam(delay, "delay.syncLeft", static_cast<float>(rate));
        setParam(delay, "delay.syncRight", static_cast<float>(rate));
        setParam(delay, "delay.feedback", 0.0f);
        setParam(delay, "delay.mix", 1.0f);
        setParam(delay, "delay.lowCut", 20.0f);
        DeviceHarness h(delay);
        feed(h, [](std::int64_t i) { return i == 100 ? 1.0f : 0.0f; });
        const auto expected = static_cast<std::int64_t>(std::llround(samples));
        h.renderTo(100 + expected + 50, 0);
        std::int64_t first = -1;
        for (std::size_t i = 0; i < h.audioLeft.size(); ++i)
            if (std::abs(h.audioLeft[i]) > 1e-3f)
            {
                first = static_cast<std::int64_t>(i);
                break;
            }
        CHECK(first == 100 + expected);
        CHECK(h.audioLeft[static_cast<std::size_t>(100 + expected)] == Catch::Approx(1.0f).margin(1e-4));
    }
}

TEST_CASE("Saturator: oversampling measurably reduces aliasing", "[devices][effects][saturator][heavy]")
{
    // A loud sine through the soft curve: its harmonics run past Nyquist, and fall off fast enough
    // that each doubling of the rate leaves far less to fold back. (Hard clipping's harmonics only
    // fall as 1/n, so there the gain from 4x over 2x is small.)
    const double hz = 4848 * sr / 65536.0; // on an FFT bin, about 3.55 kHz
    const auto aliasingWith = [&](int oversampling) {
        if (oversampling == 1)
        {
            std::vector<float> naive(1 << 17);
            for (std::size_t i = 0; i < naive.size(); ++i)
                naive[i] = SaturatorDevice::shape(SaturatorDevice::Curve::Soft,
                                                  static_cast<float>(4.0 * std::sin(2.0 * pi * hz * static_cast<double>(i) / sr)));
            return aliasingDb({naive.begin() + 4096, naive.end()}, hz, sr, 16, true, 19000.0);
        }
        SaturatorDevice sat;
        setParam(sat, "sat.curve", static_cast<float>(SaturatorDevice::Curve::Soft));
        setParam(sat, "sat.drive", 12.04f); // x4
        setParam(sat, "sat.output", 0.0f);
        setParam(sat, "sat.oversampling", oversampling == 4 ? 1.0f : 0.0f);
        DeviceHarness h(sat);
        feed(h, [hz](std::int64_t i) { return static_cast<float>(std::sin(2.0 * pi * hz * static_cast<double>(i) / sr)); });
        h.renderTo(4096 + (1 << 16));
        // Up to 19 kHz: above it the decimation filter's transition band lets a little fold back,
        // the same at either factor (and above the band anyone hears).
        return aliasingDb(h.slice(4096, 4096 + (1 << 16)), hz, sr, 16, true, 19000.0);
    };
    const double none = aliasingWith(1);
    const double two = aliasingWith(2);
    const double four = aliasingWith(4);
    INFO("aliasing: none " << none << " dB, 2x " << two << " dB, 4x " << four << " dB");
    CHECK(two < none - 10.0);
    CHECK(four < two - 6.0);
}

TEST_CASE("Saturator: the reported latency lines the wet signal up with the dry", "[devices][effects][saturator]")
{
    for (const float factor : {0.0f, 1.0f})
    {
        SaturatorDevice sat;
        setParam(sat, "sat.curve", static_cast<float>(SaturatorDevice::Curve::Soft));
        setParam(sat, "sat.drive", 0.0f);
        setParam(sat, "sat.output", 0.0f);
        setParam(sat, "sat.oversampling", factor);
        DeviceHarness h(sat);
        feed(h, [](std::int64_t i) { return i == 1000 ? 0.1f : 0.0f; }); // small: nearly linear
        h.renderTo(2000);
        std::size_t peak = 0;
        for (std::size_t i = 0; i < h.audioLeft.size(); ++i)
            if (std::abs(h.audioLeft[i]) > std::abs(h.audioLeft[peak]))
                peak = i;
        CHECK(static_cast<int>(peak) == 1000 + sat.latencySamples());
    }
}

TEST_CASE("Utility: the math is exact", "[devices][effects][utility]")
{
    UtilityDevice utility;
    setParam(utility, "utility.gain", -6.0f);
    setParam(utility, "utility.width", 1.5f);
    setParam(utility, "utility.balance", 0.25f);
    setParam(utility, "utility.invertRight", 1.0f);
    DeviceHarness h(utility);
    h.inputSignal = [](std::int64_t i) {
        return std::pair<float, float>{static_cast<float>(std::sin(0.01 * static_cast<double>(i))),
                                       static_cast<float>(std::cos(0.023 * static_cast<double>(i)))};
    };
    h.renderTo(4800);
    const float g = std::pow(10.0f, -6.0f / 20.0f);
    for (std::int64_t i = 0; i < 4800; i += 37)
    {
        const auto l = static_cast<float>(std::sin(0.01 * static_cast<double>(i)));
        const auto r = static_cast<float>(std::cos(0.023 * static_cast<double>(i)));
        const float m = 0.5f * (l + r);
        const float s = 0.5f * (l - r) * 1.5f;
        const float expectedL = (m + s) * g * 0.75f; // balance right: left at 75 %
        const float expectedR = -(m - s) * g;
        CHECK(h.audioLeft[static_cast<std::size_t>(i)] == Catch::Approx(expectedL).margin(1e-6));
        CHECK(h.audioRight[static_cast<std::size_t>(i)] == Catch::Approx(expectedR).margin(1e-6));
    }
}

TEST_CASE("Limiter: the output never goes over the ceiling, and its latency is reported", "[devices][effects][dynamics]")
{
    LimiterDevice limiter;
    setParam(limiter, "limiter.ceiling", -1.0f);
    setParam(limiter, "limiter.gain", 12.0f);
    DeviceHarness h(limiter);
    std::uint32_t seed = 3;
    h.inputSignal = [&seed](std::int64_t i) {
        seed = seed * 1664525u + 1013904223u;
        const float noise = static_cast<float>(seed >> 8) / 8388608.0f - 1.0f;
        const float burst = (i / 4800) % 3 == 0 ? 4.0f : 0.3f; // loud bursts, quiet gaps
        const float spike = i % 7919 == 0 ? 20.0f : 0.0f;       // and single-sample spikes
        return std::pair<float, float>{noise * burst + spike, -noise * burst * 0.7f};
    };
    h.renderTo(48000 * 3, 0);
    const float ceiling = std::pow(10.0f, -1.0f / 20.0f);
    CHECK(peakOf(h.audioLeft) <= ceiling);
    CHECK(peakOf(h.audioRight) <= ceiling);
    CHECK(limiter.latencySamples() == static_cast<int>(std::lround(LimiterDevice::lookaheadSeconds * sr)));
    CHECK(peakOf(h.audioLeft) > 0.9f * ceiling); // it is loud, just not too loud
}

// --- Reverb ------------------------------------------------------------------------------------------------

namespace
{
/** The decay time (s) of a signal's energy in a band: Schroeder backward integration of the
    band-passed impulse response, fitted from -5 to -35 dB. */
double measuredRt60(const std::vector<float>& ir, double centreHz)
{
    // Band-pass with a matched biquad, twice (steeper).
    const auto c = dsp::BiquadCoefficients::design(dsp::BiquadType::BandPass, centreHz, 2.0, 0.0, sr);
    dsp::Biquad a, b;
    a.set(c);
    b.set(c);
    std::vector<double> energy(ir.size());
    for (std::size_t i = 0; i < ir.size(); ++i)
    {
        const float y = b.process(a.process(ir[i]));
        energy[i] = static_cast<double>(y) * y;
    }
    // Backward integration.
    std::vector<double> curve(ir.size());
    double sum = 0.0;
    for (std::size_t i = ir.size(); i-- > 0;)
    {
        sum += energy[i];
        curve[i] = sum;
    }
    const double total = curve[0];
    std::size_t start = 0;
    std::size_t end = 0;
    for (std::size_t i = 0; i < curve.size(); ++i)
    {
        const double db = 10.0 * std::log10(curve[i] / total);
        if (start == 0 && db <= -5.0)
            start = i;
        if (db <= -35.0)
        {
            end = i;
            break;
        }
    }
    REQUIRE(end > start);
    // Least-squares slope (dB per sample) between them.
    double sx = 0, sy = 0, sxx = 0, sxy = 0;
    int count = 0;
    for (std::size_t i = start; i < end; i += 16)
    {
        const double x = static_cast<double>(i);
        const double y = 10.0 * std::log10(curve[i] / total);
        sx += x;
        sy += y;
        sxx += x * x;
        sxy += x * y;
        ++count;
    }
    const double slope = (count * sxy - sx * sy) / (count * sxx - sx * sx);
    return -60.0 / slope / sr;
}

std::vector<float> reverbImpulse(ReverbDevice& reverb, double seconds, std::vector<float>* right = nullptr)
{
    setParam(reverb, "reverb.mix", 1.0f);
    setParam(reverb, "reverb.early", -70.0f); // the late tail alone
    setParam(reverb, "reverb.predelay", 0.0f);
    setParam(reverb, "reverb.lowCut", 20.0f);
    setParam(reverb, "reverb.highCut", 20000.0f);
    DeviceHarness h(reverb);
    h.inputSignal = [](std::int64_t i) { return std::pair<float, float>{i == 0 ? 1.0f : 0.0f, i == 0 ? 1.0f : 0.0f}; };
    h.renderTo(static_cast<std::int64_t>(seconds * sr));
    if (right != nullptr)
        *right = h.audioRight;
    return h.audioLeft;
}
} // namespace

TEST_CASE("Reverb: each band decays in its set time (RT60 within 10 %)", "[devices][effects][reverb][heavy]")
{
    for (const int quality : {1, 0})
    {
        ReverbDevice reverb;
        setParam(reverb, "reverb.quality", static_cast<float>(quality));
        setParam(reverb, "reverb.decay", 2.0f);
        setParam(reverb, "reverb.lowDecay", 2.0f);  // 4 s below 250 Hz
        setParam(reverb, "reverb.highDecay", 0.4f); // 0.8 s above 4 kHz
        setParam(reverb, "reverb.lowCrossover", 250.0f);
        setParam(reverb, "reverb.highCrossover", 4000.0f);
        const auto ir = reverbImpulse(reverb, 8.0);
        INFO("quality " << quality);
        const double low = measuredRt60(ir, 60.0);
        const double mid = measuredRt60(ir, 1000.0);
        const double high = measuredRt60(ir, 14000.0);
        INFO("low " << low << " s, mid " << mid << " s, high " << high << " s");
        CHECK(low == Catch::Approx(4.0).epsilon(0.10));
        CHECK(mid == Catch::Approx(2.0).epsilon(0.10));
        CHECK(high == Catch::Approx(0.8).epsilon(0.10));
    }
}

TEST_CASE("Reverb: no strong resonances, and decorrelated channels", "[devices][effects][reverb][heavy]")
{
    ReverbDevice reverb;
    setParam(reverb, "reverb.decay", 3.0f);
    setParam(reverb, "reverb.lowDecay", 1.0f);
    setParam(reverb, "reverb.highDecay", 1.0f);
    setParam(reverb, "reverb.width", 1.0f);
    std::vector<float> right;
    const auto left = reverbImpulse(reverb, 4.0, &right);

    // The tail's spectrum (0.3 s to 3.3 s) averaged over many short segments, each scaled to the
    // same energy so the decay doesn't weight them (Welch): averaging removes the random spikiness
    // of a single spectrum, so what still stands out is a real resonance.
    constexpr int order = 12;
    constexpr int size = 1 << order;
    std::vector<double> power(size / 2, 0.0);
    int segments = 0;
    for (std::size_t start = 14400; start + size <= 14400 + static_cast<std::size_t>(3.0 * sr); start += size / 2)
    {
        const std::vector<float> segment(left.begin() + static_cast<std::ptrdiff_t>(start),
                                         left.begin() + static_cast<std::ptrdiff_t>(start + size));
        const auto a = amplitudeSpectrum(segment, order);
        double energy = 0.0;
        for (const double v : a)
            energy += v * v;
        for (std::size_t i = 0; i < a.size(); ++i)
            power[i] += a[i] * a[i] / energy;
        ++segments;
    }
    double worst = -100.0;
    const int smooth = 16; // bins: about 190 Hz either side
    const double binHz = sr / size;
    for (int bin = static_cast<int>(100.0 / binHz); bin + smooth < static_cast<int>(power.size()) && bin * binHz < 12000.0; ++bin)
    {
        double sum = 0.0;
        for (int k = bin - smooth; k <= bin + smooth; ++k)
            sum += power[static_cast<std::size_t>(k)];
        worst = std::max(worst, 10.0 * std::log10(power[static_cast<std::size_t>(bin)] * (2 * smooth + 1) / sum));
    }
    INFO(segments << " segments; strongest peak over the smoothed spectrum: " << worst << " dB");
    CHECK(worst < 6.0);

    // Left and right at full width.
    double lr = 0.0, ll = 0.0, rr = 0.0;
    for (std::size_t i = 2400; i < left.size(); ++i)
    {
        lr += static_cast<double>(left[i]) * right[i];
        ll += static_cast<double>(left[i]) * left[i];
        rr += static_cast<double>(right[i]) * right[i];
    }
    const double correlation = lr / std::sqrt(ll * rr);
    INFO("correlation " << correlation);
    CHECK(std::abs(correlation) < 0.3);
}

TEST_CASE("Reverb: freeze holds the level", "[devices][effects][reverb][heavy]")
{
    ReverbDevice reverb;
    setParam(reverb, "reverb.mix", 1.0f);
    setParam(reverb, "reverb.early", -70.0f);
    DeviceHarness h(reverb);
    std::uint32_t seed = 9;
    h.inputSignal = [&seed](std::int64_t i) {
        seed = seed * 1664525u + 1013904223u;
        const float v = i < 24000 ? 0.3f * (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) : 0.0f;
        return std::pair<float, float>{v, v};
    };
    h.renderTo(48000);
    setParam(reverb, "reverb.freeze", 1.0f);
    h.renderTo(48000 * 12);
    const double early = rms(h.audioLeft, 48000 * 2, 48000 * 3);
    const double late = rms(h.audioLeft, 48000 * 11, 48000 * 12);
    INFO("level 1 s into freeze " << toDb(early) << " dB, 10 s later " << toDb(late) << " dB");
    REQUIRE(early > 1e-3);
    CHECK(std::abs(toDb(late / early)) < 1.0);
}

TEST_CASE("Reverb: a long silent tail costs no more than sound (no denormals)", "[devices][effects][reverb][timing]")
{
    ReverbDevice reverb;
    setParam(reverb, "reverb.decay", 1.0f);
    DeviceHarness h(reverb);
    h.inputSignal = [](std::int64_t i) {
        const float v = i < 48000 ? static_cast<float>(0.5 * std::sin(0.05 * static_cast<double>(i))) : 0.0f;
        return std::pair<float, float>{v, v};
    };
    const auto timeFor = [&](std::int64_t until) {
        const auto t0 = std::chrono::steady_clock::now();
        h.renderTo(until, 256);
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    };
    const double sounding = timeFor(48000 * 2);
    timeFor(48000 * 50);
    const double silent = timeFor(48000 * 62) / 6.0; // 12 s, per 2 s
    INFO("2 s with sound " << sounding << " s, 2 s of silent tail " << silent << " s");
    CHECK(silent < 2.0 * sounding);
}
