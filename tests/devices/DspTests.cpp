// Shared DSP building blocks (docs/04-m3-devices.md, "Shared DSP building blocks").

#include "devices/dsp/Biquad.h"
#include "devices/dsp/DelayLine.h"
#include "devices/dsp/Envelope.h"
#include "devices/dsp/HalfBand.h"
#include "devices/dsp/Ladder.h"
#include "devices/dsp/Svf.h"
#include "devices/dsp/Wavetable.h"
#include "support/Spectrum.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

using namespace lattice;
using namespace lattice::test;

namespace
{
constexpr double sr = 48000.0;

std::vector<float> renderWave(const dsp::MipmappedWave& wave, double hz, int numSamples, double pulseWidth = -1.0)
{
    std::vector<float> out(static_cast<std::size_t>(numSamples));
    const auto inc = dsp::incrementFor(hz, sr);
    const auto at = dsp::MipmappedWave::positionFor(inc);
    const auto offset = static_cast<std::uint32_t>(pulseWidth * 4294967296.0);
    std::uint32_t phase = 0x12345678u;
    for (auto& v : out)
    {
        v = pulseWidth < 0.0 ? wave.sample(phase, at) : wave.sample(phase, at) - wave.sample(phase + offset, at);
        phase += inc;
    }
    return out;
}

/** Frequencies from 20 Hz to 15 kHz, `perOctave` per octave, slightly detuned so they land
    anywhere within the tables' ranges (including the crossfades). */
std::vector<double> sweepFrequencies(int perOctave)
{
    std::vector<double> f;
    for (double hz = 20.0; hz <= 15000.0; hz *= std::exp2(1.0 / perOctave))
        f.push_back(hz * 1.0123);
    f.push_back(15000.0);
    return f;
}

void checkAliasing(const dsp::MipmappedWave& wave, const char* name, int perOctave, double pulseWidth = -1.0)
{
    double worst = -400.0;
    double worstAt = 0.0;
    for (const double hz : sweepFrequencies(perOctave))
    {
        const auto signal = renderWave(wave, hz, 1 << 16, pulseWidth);
        const double a = aliasingDb(signal, hz);
        if (a > worst)
        {
            worst = a;
            worstAt = hz;
        }
    }
    INFO(name << ": worst aliasing " << worst << " dB at " << worstAt << " Hz");
    CHECK(worst <= -80.0);
}
} // namespace

TEST_CASE("Oscillators: saw, triangle and pulse alias at or below -80 dB from 20 Hz to 15 kHz",
          "[devices][dsp][oscillator]")
{
    const auto& lib = dsp::WaveformLibrary::get();
    checkAliasing(lib.shape(dsp::WaveformLibrary::Shape::Saw), "saw", 3);
    checkAliasing(lib.shape(dsp::WaveformLibrary::Shape::Triangle), "triangle", 3);
    checkAliasing(lib.shape(dsp::WaveformLibrary::Shape::Saw), "square (two saws)", 3, 0.5);
    checkAliasing(lib.shape(dsp::WaveformLibrary::Shape::Saw), "pulse 20%", 3, 0.2);
}

TEST_CASE("Oscillators: the full sweep at 1/12 octave", "[devices][dsp][oscillator][slow]")
{
    const auto& lib = dsp::WaveformLibrary::get();
    checkAliasing(lib.shape(dsp::WaveformLibrary::Shape::Saw), "saw", 12);
    checkAliasing(lib.shape(dsp::WaveformLibrary::Shape::Triangle), "triangle", 12);
    checkAliasing(lib.shape(dsp::WaveformLibrary::Shape::Saw), "pulse 30%", 12, 0.3);
}

TEST_CASE("Oscillators: the band-limited saw keeps its level and has its fundamental where expected",
          "[devices][dsp][oscillator]")
{
    const auto& saw = dsp::WaveformLibrary::get().shape(dsp::WaveformLibrary::Shape::Saw);
    // Exactly on FFT bins, so the window's scalloping doesn't lower the measured peak.
    for (const int fftBin : {75, 601, 4806}) // about 55, 440 and 3520 Hz
    {
        const double hz = fftBin * sr / 65536.0;
        INFO(hz << " Hz");
        const auto signal = renderWave(saw, hz, 1 << 16);
        const auto spectrum = amplitudeSpectrum(signal, 16);
        const auto bin = static_cast<std::size_t>(std::round(hz / (sr / 65536.0)));
        double fundamental = 0.0;
        for (auto b = bin - 3; b <= bin + 3; ++b)
            fundamental = std::max(fundamental, spectrum[b]);
        // The richest table peaks at 1 including its Gibbs overshoot (about 1.179 times a ramp from
        // -1 to 1), so the fundamental is (2 / pi) / 1.179 at every pitch.
        CHECK(fundamental == Catch::Approx(2.0 / juce::MathConstants<double>::pi / 1.179).epsilon(0.02));
    }
    CHECK(saw.harmonicsAt(0) == 2048);
    CHECK(saw.harmonicsAt(dsp::MipmappedWave::numLevels - 1) == 1);
}

TEST_CASE("Oscillators: the position within the tables is continuous in pitch", "[devices][dsp][oscillator]")
{
    // Walking up in small steps, the effective level (level + blend) never jumps.
    double previous = -1.0;
    for (double hz = 10.0; hz < 23900.0; hz *= 1.001)
    {
        const auto at = dsp::MipmappedWave::positionFor(dsp::incrementFor(hz, sr));
        const double effective = at.level + at.blend;
        if (previous >= 0.0)
            CHECK(effective - previous < 0.02);
        CHECK(effective >= previous - 1e-6);
        previous = effective;
    }
    CHECK(dsp::MipmappedWave::positionFor(dsp::incrementFor(24500.0, sr)).silent);
}

// --- Filters -------------------------------------------------------------------------------------------

namespace
{
/** Steady-state gain of a per-sample process at a frequency: feed a sine, skip the transient,
    compare RMS. */
template <typename Process>
double measuredGain(Process&& process, double hz, int settle = 24000, int measure = 48000)
{
    double inPower = 0.0;
    double outPower = 0.0;
    for (int i = 0; i < settle + measure; ++i)
    {
        const auto x = static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * hz * i / sr));
        const float y = process(x);
        if (i >= settle)
        {
            inPower += static_cast<double>(x) * x;
            outPower += static_cast<double>(y) * y;
        }
    }
    return std::sqrt(outPower / inPower);
}

double toDb(double gain)
{
    return 20.0 * std::log10(std::max(gain, 1e-12));
}

/** The frequency (by bisection on a log scale between lo and hi) where a falling response crosses
    targetDb. make() returns a fresh per-sample process each time. */
template <typename MakeProcess>
double crossing(MakeProcess&& make, double targetDb, double lo, double hi)
{
    for (int i = 0; i < 30; ++i)
    {
        const double mid = std::sqrt(lo * hi);
        auto p = make();
        if (toDb(measuredGain(p, mid, 4800, 9600)) > targetDb)
            lo = mid;
        else
            hi = mid;
    }
    return std::sqrt(lo * hi);
}
} // namespace

TEST_CASE("SVF: the measured cutoff is within 3% of the setting", "[devices][dsp][filter]")
{
    for (const double fc : {100.0, 1000.0, 8000.0, 16000.0})
    {
        INFO(fc << " Hz");
        const float g = dsp::SvfCoefficients::prewarp(fc, sr);
        const double hi = std::min(fc * 2.0, 23000.0);
        // 12 dB, resonance 0 (Q = 0.707): -3 dB at the cutoff.
        const double at12 = crossing(
            [&] {
                return [f = dsp::Svf{}, g](float x) mutable {
                    f.setCoefficients(g, 0.0f, false);
                    return f.process(x, dsp::SvfMode::LowPass);
                };
            },
            -3.0103, fc / 2.0, hi);
        CHECK(at12 == Catch::Approx(fc).epsilon(0.03));
        // 24 dB Butterworth: -3 dB at the cutoff too.
        const double at24 = crossing(
            [&] {
                return [f = dsp::Svf{}, g](float x) mutable {
                    f.setCoefficients(g, 0.0f, true);
                    return f.process(x, dsp::SvfMode::LowPass);
                };
            },
            -3.0103, fc / 2.0, hi);
        CHECK(at24 == Catch::Approx(fc).epsilon(0.03));
    }
}

TEST_CASE("Ladder: cutoff within 3%, and stable at full resonance", "[devices][dsp][filter]")
{
    for (const double fc : {200.0, 2000.0, 10000.0})
    {
        INFO(fc << " Hz");
        const float g = dsp::LadderFilter::prewarpOversampled(fc, sr);
        // Four poles at the cutoff, no resonance: -12 dB there.
        const double at = crossing(
            [&] {
                return [f = dsp::LadderFilter{}, g](float x) mutable {
                    f.setCoefficients(g, 0.0f, 1.0f);
                    return f.process(x * 0.01f) * 100.0f; // small signal: linear
                };
            },
            -12.0412, fc / 2.0, std::min(fc * 2.0, 23000.0));
        CHECK(at == Catch::Approx(fc).epsilon(0.03));
    }

    // Full resonance with a loud input, then silence: bounded while driven and while it rings on,
    // and never NaN or Inf.
    dsp::LadderFilter f;
    f.setCoefficients(dsp::LadderFilter::prewarpOversampled(1000.0, sr), 1.0f, 4.0f);
    float largest = 0.0f;
    for (int i = 0; i < 96000; ++i)
    {
        const float x = i < 48000 ? (i % 200 < 100 ? 1.0f : -1.0f) : 0.0f;
        const float y = f.process(x);
        REQUIRE(std::isfinite(y));
        largest = std::max(largest, std::abs(y));
    }
    CHECK(largest < 8.0f);
}

TEST_CASE("Half-band: flat pass band, deep stop band", "[devices][dsp][filter]")
{
    const auto design = dsp::HalfBandDesign::make(80.0, 0.08);
    INFO(design.coefficients.size() << " coefficients");
    CHECK(design.coefficients.size() <= 12);
    // Upsample a tone; its image at fs - f (in the 2 fs band) must be at least 80 dB down.
    // On FFT bins (2^17 points at 96 kHz), so the window's scalloping doesn't distort the levels.
    for (const int fftBin : {1365, 10923, 20480}) // about 1, 8 and 15 kHz
    {
        const double hz = fftBin * 2.0 * sr / (1 << 17);
        INFO(hz << " Hz");
        dsp::HalfBand2x hb(design);
        std::vector<float> up;
        for (int i = 0; i < 1 << 16; ++i)
        {
            float a = 0.0f;
            float b = 0.0f;
            hb.upsample(static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * hz * i / sr)), a, b);
            up.push_back(a);
            up.push_back(b);
        }
        const auto spectrum = amplitudeSpectrum(up, 17); // at 96 kHz
        const double binHz = 2.0 * sr / (1 << 17);
        const auto level = [&](double f) {
            double m = 0.0;
            const int b = static_cast<int>(std::round(f / binHz));
            for (int i = b - 4; i <= b + 4; ++i)
                m = std::max(m, spectrum[static_cast<std::size_t>(i)]);
            return m;
        };
        CHECK(toDb(level(hz)) == Catch::Approx(0.0).margin(0.1));
        CHECK(toDb(level(sr - hz)) < -80.0);
    }
    // Down: a tone in the upper half of the oversampled band is removed.
    dsp::HalfBand2x hb(design);
    double out = 0.0;
    for (int i = 0; i < 48000; ++i)
    {
        const auto s = [&](double n) {
            return static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * 30000.0 * n / (2.0 * sr)));
        };
        const float y = hb.downsample(s(2.0 * i), s(2.0 * i + 1.0));
        if (i > 4800)
            out = std::max(out, static_cast<double>(std::abs(y)));
    }
    CHECK(toDb(out) < -80.0);
}

TEST_CASE("Biquads: matched designs follow the analog prototype up to Nyquist", "[devices][dsp][filter]")
{
    using dsp::BiquadType;
    const auto worstError = [](const dsp::BiquadCoefficients& c, BiquadType type, double fc, double q, double gain) {
        double worst = 0.0;
        for (double f = 20.0; f < 0.95 * sr / 2.0; f *= 1.03)
        {
            const double analog = toDb(std::abs(dsp::BiquadCoefficients::analogResponse(type, f, fc, q, gain)));
            if (analog > -20.0) // relative error means little far down the skirts
                worst = std::max(worst, std::abs(toDb(std::abs(c.response(f, sr))) - analog));
        }
        return worst;
    };
    for (const auto type : {BiquadType::LowPass, BiquadType::HighPass, BiquadType::BandPass, BiquadType::Peak})
    {
        for (const double fc : {100.0, 1000.0, 5000.0, 10000.0, 18000.0})
        {
            for (const double q : {0.5, 0.7071, 3.0})
            {
                for (const double gain : {-12.0, 9.0})
                {
                    INFO("type " << static_cast<int>(type) << ", " << fc << " Hz, Q " << q << ", " << gain << " dB");
                    const auto matched = dsp::BiquadCoefficients::design(type, fc, q, gain, sr);
                    const auto bilinear = dsp::BiquadCoefficients::designBilinear(type, fc, q, gain, sr);
                    const double matchedError = worstError(matched, type, fc, q, gain);
                    const double bilinearError = worstError(bilinear, type, fc, q, gain);
                    INFO("matched " << matchedError << " dB, bilinear " << bilinearError << " dB");
                    // A second-order section can't follow an analog response all the way to
                    // Nyquist; the matched design stays within about a dB (the widest filters are
                    // the hardest), and is always at least as close as the bilinear one.
                    if (fc <= 5000.0)
                        CHECK(matchedError < 1.2);
                    else if (fc <= 12000.0)
                        CHECK(matchedError < 2.0);
                    CHECK(matchedError <= bilinearError + 0.05);
                    const double analogAtCentre =
                        toDb(std::abs(dsp::BiquadCoefficients::analogResponse(type, fc, fc, q, gain)));
                    CHECK(toDb(std::abs(matched.response(fc, sr))) == Catch::Approx(analogAtCentre).margin(0.01));
                }
            }
        }
    }
}

TEST_CASE("Biquads: the measured response is the analytic one within 0.1 dB", "[devices][dsp][filter]")
{
    using dsp::BiquadType;
    for (const auto type : {BiquadType::Peak, BiquadType::LowShelf, BiquadType::HighShelf, BiquadType::Notch,
                            BiquadType::LowPass, BiquadType::HighPass})
    {
        const auto c = dsp::BiquadCoefficients::design(type, 1200.0, 1.0, 6.0, sr);
        for (const double f : {50.0, 400.0, 1100.0, 5000.0, 15000.0})
        {
            INFO("type " << static_cast<int>(type) << " at " << f << " Hz");
            dsp::Biquad b;
            b.set(c);
            const double measured = toDb(measuredGain([&](float x) { return b.process(x); }, f));
            CHECK(measured == Catch::Approx(toDb(std::abs(c.response(f, sr)))).margin(0.1));
        }
    }
}

// --- Envelopes, delay lines ----------------------------------------------------------------------------

TEST_CASE("ADSR: segment times are accurate to within 1 ms", "[devices][dsp][envelope]")
{
    for (const float curve : {0.0f, 0.5f, 1.0f})
    {
        INFO("curve " << curve);
        dsp::Adsr env;
        env.setSampleRate(sr);
        dsp::Adsr::Settings s;
        s.attackSeconds = 0.010f;
        s.decaySeconds = 0.200f;
        s.sustain = 0.5f;
        s.releaseSeconds = 0.500f;
        s.attackCurve = curve;
        s.decayCurve = curve;
        env.setSettings(s);
        env.noteOn();
        int peakAt = -1;
        int sustainAt = -1;
        for (int i = 0; i < 48000; ++i)
        {
            const float v = env.next();
            if (peakAt < 0 && v >= 0.9999f)
                peakAt = i + 1;
            if (peakAt >= 0 && sustainAt < 0 && env.currentStage() == dsp::Adsr::Stage::Sustain)
                sustainAt = i + 1;
        }
        CHECK(std::abs(peakAt - 480) <= 48);
        CHECK(std::abs(sustainAt - (480 + 9600)) <= 48);
        CHECK(env.currentValue() == Catch::Approx(0.5f).margin(1e-4));

        env.noteOff();
        int silentAt = -1;
        for (int i = 0; i < 48000 && silentAt < 0; ++i)
        {
            env.next();
            if (!env.isActive())
                silentAt = i + 1;
        }
        CHECK(std::abs(silentAt - 24000) <= 48);
    }
}

TEST_CASE("Delay line: integer delays are exact, fractional ones accurate", "[devices][dsp]")
{
    dsp::DelayLine d;
    d.prepare(1000);
    for (int i = 0; i < 2000; ++i)
    {
        d.push(static_cast<float>(i));
        if (i >= 1000)
        {
            CHECK(d.tap(37) == static_cast<float>(i - 37));
            CHECK(d.read(37.0f) == Catch::Approx(i - 37.0f));
            CHECK(d.read(37.25f) == Catch::Approx(i - 37.25f).margin(1e-3)); // a ramp is reproduced exactly
        }
    }
    // A sine delayed by a fraction keeps its amplitude and phase.
    dsp::DelayLine s;
    s.prepare(100);
    double worst = 0.0;
    for (int i = 0; i < 4800; ++i)
    {
        s.push(static_cast<float>(std::sin(2.0 * juce::MathConstants<double>::pi * 1000.0 * i / sr)));
        if (i > 100)
        {
            const double expected = std::sin(2.0 * juce::MathConstants<double>::pi * 1000.0 * (i - 10.5) / sr);
            worst = std::max(worst, std::abs(s.read(10.5f) - expected));
        }
    }
    CHECK(worst < 1e-3);
}
