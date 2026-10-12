#include "devices/dsp/Wavetable.h"

#include <juce_dsp/juce_dsp.h>

#include <cmath>
#include <future>
#include <mutex>

namespace lattice::dsp
{
namespace
{
constexpr double pi = 3.14159265358979323846;

/** The top increment (cycles per sample) of a level: 2^((k - 26) / 2). */
double levelTop(int level)
{
    return std::exp2((level - 26) * 0.5);
}

int nextPowerOfTwoBits(int value)
{
    int bits = 0;
    while ((1 << bits) < value)
        ++bits;
    return bits;
}
} // namespace

MipmappedWave::MipmappedWave(const Spectrum& spectrum, int maxHarmonics, int oversampling, double peak)
{
    double gain = 0.0; // set from the richest level, then used for all, so levels match in level
    for (int k = 0; k < numLevels; ++k)
    {
        auto& level = levels[static_cast<std::size_t>(k)];
        const int harmonics = std::max(1, std::min(maxHarmonics, static_cast<int>(std::floor(0.5 / levelTop(k)))));
        const int bits = juce::jlimit(minTableBits, 15, nextPowerOfTwoBits(oversampling * harmonics));
        const int size = 1 << bits;
        level.bits = bits;
        level.harmonics = harmonics;

        // Inverse FFT of the truncated spectrum. x(n) = sum Re(C_h e^(i 2 pi h n / N)), C_h = a - i b.
        const juce::dsp::FFT fft(bits);
        std::vector<juce::dsp::Complex<float>> bins(static_cast<std::size_t>(size));
        std::vector<juce::dsp::Complex<float>> out(static_cast<std::size_t>(size));
        double expectedPower = 0.0;
        for (int h = 1; h <= harmonics && h < size / 2; ++h)
        {
            const auto c = spectrum(h);
            const std::complex<double> ch(c.real(), -c.imag());
            bins[static_cast<std::size_t>(h)] = {static_cast<float>(0.5 * ch.real()), static_cast<float>(0.5 * ch.imag())};
            bins[static_cast<std::size_t>(size - h)] = {static_cast<float>(0.5 * ch.real()),
                                                        static_cast<float>(-0.5 * ch.imag())};
            expectedPower += 0.5 * std::norm(c);
        }
        fft.perform(bins.data(), out.data(), true);

        // Whatever scaling the FFT applies, Parseval fixes it: the cycle's mean power is known.
        double power = 0.0;
        for (const auto& v : out)
            power += static_cast<double>(v.real()) * v.real();
        power /= size;
        const double fix = power > 0.0 ? std::sqrt(expectedPower / power) : 0.0;

        std::vector<double> cycle(static_cast<std::size_t>(size));
        double largest = 0.0;
        for (int n = 0; n < size; ++n)
        {
            cycle[static_cast<std::size_t>(n)] = out[static_cast<std::size_t>(n)].real() * fix;
            largest = std::max(largest, std::abs(cycle[static_cast<std::size_t>(n)]));
        }
        if (k == 0)
            gain = largest > 0.0 ? peak / largest : 0.0;

        level.table.resize(static_cast<std::size_t>(size + 2 * guard));
        for (int i = -guard; i < size + guard; ++i)
        {
            const int wrapped = (i % size + size) % size;
            level.table[static_cast<std::size_t>(i + guard)] =
                static_cast<float>(cycle[static_cast<std::size_t>(wrapped)] * gain);
        }
    }
}

MipmappedWave::Position MipmappedWave::positionFor(std::uint32_t increment) noexcept
{
    Position at;
    if (increment == 0)
        return at;
    // x = 2 log2(cycles) + 26: 0 at 2^-13 cycles per sample, 24 at Nyquist.
    const float x = 2.0f * std::log2(static_cast<float>(increment)) - 38.0f;
    if (x <= 0.0f)
        return at;
    if (x >= static_cast<float>(numLevels - 1)) // the fundamental at or above Nyquist
    {
        at.silent = true;
        return at;
    }
    const float levelF = std::ceil(x);
    at.level = static_cast<int>(levelF);
    const float within = x - (levelF - 1.0f); // (0, 1] across this level's range
    at.blend = within > 0.5f ? (within - 0.5f) * 2.0f : 0.0f;
    return at;
}

std::size_t MipmappedWave::memoryBytes() const noexcept
{
    std::size_t bytes = 0;
    for (const auto& level : levels)
        bytes += level.table.size() * sizeof(float);
    return bytes;
}

// --- Wavetable ------------------------------------------------------------------------------------

Wavetable::Wavetable(juce::String name, std::vector<std::unique_ptr<MipmappedWave>> f)
    : tableName(std::move(name)), frames(std::move(f))
{
    jassert(!frames.empty());
}

// --- Library ----------------------------------------------------------------------------------------

namespace
{
std::complex<double> sawHarmonic(int h)
{
    return {0.0, -2.0 / (pi * h)}; // 2x - 1 over the cycle: a rising ramp
}

std::complex<double> triangleHarmonic(int h)
{
    if (h % 2 == 0)
        return {};
    const double sign = ((h - 1) / 2) % 2 == 0 ? 1.0 : -1.0;
    return {0.0, sign * 8.0 / (pi * pi * h * h)};
}

std::complex<double> pulseHarmonic(int h, double width)
{
    // A pulse that is high for `width` of the cycle, without its DC.
    return {std::sin(2.0 * pi * h * width) * 2.0 / (pi * h), (1.0 - std::cos(2.0 * pi * h * width)) * 2.0 / (pi * h)};
}

using FrameSpectrum = std::function<std::complex<double>(int harmonic, double t)>;

std::unique_ptr<Wavetable> makeTable(const juce::String& name, int numFrames, const FrameSpectrum& spectrum)
{
    constexpr int maxHarmonics = 1024;
    constexpr int oversampling = 8;
    std::vector<std::unique_ptr<MipmappedWave>> frames;
    for (int f = 0; f < numFrames; ++f)
    {
        const double t = numFrames > 1 ? static_cast<double>(f) / (numFrames - 1) : 0.0;
        frames.push_back(std::make_unique<MipmappedWave>([&](int h) { return spectrum(h, t); }, maxHarmonics,
                                                         oversampling, 0.9));
    }
    return std::make_unique<Wavetable>(name, std::move(frames));
}

std::shared_future<const WaveformLibrary*>& pending()
{
    static std::shared_future<const WaveformLibrary*> f;
    return f;
}
std::mutex& pendingMutex()
{
    static std::mutex m;
    return m;
}
} // namespace

WaveformLibrary::WaveformLibrary()
{
    shapes[static_cast<std::size_t>(Shape::Sine)] =
        std::make_unique<MipmappedWave>([](int h) { return h == 1 ? std::complex<double>(0.0, 1.0) : std::complex<double>(); },
                                        1, 16, 1.0);
    shapes[static_cast<std::size_t>(Shape::Triangle)] = std::make_unique<MipmappedWave>(triangleHarmonic, 2048, 16, 1.0);
    shapes[static_cast<std::size_t>(Shape::Saw)] = std::make_unique<MipmappedWave>(sawHarmonic, 2048, 16, 1.0);

    constexpr int frames = 8;
    // Sine to triangle to saw to square: the classic shapes, morphed by their spectra.
    wavetables.push_back(makeTable("Analog", frames, [](int h, double t) {
        const double x = t * 3.0;
        const auto sine = h == 1 ? std::complex<double>(0.0, 1.0) : std::complex<double>();
        const auto tri = triangleHarmonic(h);
        const auto saw = -sawHarmonic(h);
        const auto square = pulseHarmonic(h, 0.5) * 0.5;
        if (x < 1.0)
            return sine + (tri - sine) * x;
        if (x < 2.0)
            return tri + (saw - tri) * (x - 1.0);
        return saw + (square - saw) * (x - 2.0);
    }));
    // A saw through a resonant formant that sweeps up the harmonics.
    wavetables.push_back(makeTable("Formant", frames, [](int h, double t) {
        const double centre = 2.0 * std::pow(24.0, t); // harmonic 2 .. 48
        const double d = std::log2(h / centre);
        const double formant = std::exp(-d * d * 4.0) * 4.0 + 0.15;
        return std::complex<double>(0.0, formant / h);
    }));
    // Organ drawbars: from a pure 8' to a full registration.
    wavetables.push_back(makeTable("Organ", frames, [](int h, double t) {
        static constexpr std::array<int, 9> bars{1, 2, 3, 4, 6, 8, 10, 12, 16};
        double a = 0.0;
        for (std::size_t i = 0; i < bars.size(); ++i)
        {
            if (bars[i] != h)
                continue;
            const double opens = juce::jlimit(0.0, 1.0, t * 9.0 - static_cast<double>(i) + 1.0);
            a = i == 0 ? 1.0 : 0.7 * opens / std::sqrt(static_cast<double>(h));
        }
        return std::complex<double>(0.0, a);
    }));
    // Pulse width from square to a thin pulse.
    wavetables.push_back(makeTable("Pulse Width", frames, [](int h, double t) {
        return pulseHarmonic(h, 0.5 - 0.45 * t) * 0.5;
    }));
    // The spectrum of a hard-synced saw (slave/master ratio 1 .. 6), free of the sync's aliasing.
    wavetables.push_back(makeTable("Sync", frames, [](int h, double t) {
        const double ratio = 1.0 + 5.0 * t;
        // Fourier series of y(x) = 2 frac(ratio x) - 1 over one master cycle.
        const double w = 2.0 * pi * ratio;
        const double k = 2.0 * pi * h;
        // Integral of (2 frac(r x) - 1) e^{-i k x} dx over [0, 1], computed per slave period.
        std::complex<double> sum;
        const int whole = static_cast<int>(std::floor(ratio));
        const auto segment = [&](double x0, double x1) {
            // On [x0, x1] the slave is y = 2 (r x - n) - 1 with n = floor(r x0).
            const double n = std::floor(ratio * x0 + 1e-12);
            const auto F = [&](double x) {
                const std::complex<double> e = std::exp(std::complex<double>(0.0, -k * x));
                const double y = 2.0 * (ratio * x - n) - 1.0;
                // integral of y e^{-ikx}: y e^{-ikx}/(-ik) - (2r) e^{-ikx}/(-ik)^2
                const std::complex<double> ik(0.0, -k);
                return y * e / ik - 2.0 * ratio * e / (ik * ik);
            };
            sum += F(x1) - F(x0);
        };
        for (int i = 0; i < whole; ++i)
            segment(i / ratio, (i + 1) / ratio);
        if (whole / ratio < 1.0)
            segment(whole / ratio, 1.0);
        juce::ignoreUnused(w);
        // y = sum over h of a cos + b sin: a = 2 Re(c), b = -2 Im(c)
        return std::complex<double>(2.0 * sum.real(), -2.0 * sum.imag()) * 0.6;
    }));
}

const WaveformLibrary& WaveformLibrary::get()
{
    std::shared_future<const WaveformLibrary*> f;
    {
        const std::scoped_lock lock(pendingMutex());
        if (!pending().valid())
            pending() = std::async(std::launch::deferred, [] {
                            static const WaveformLibrary library;
                            return &library;
                        }).share();
        f = pending();
    }
    return *f.get();
}

void WaveformLibrary::preloadInBackground()
{
    const std::scoped_lock lock(pendingMutex());
    if (!pending().valid())
        pending() = std::async(std::launch::async, [] {
                        static const WaveformLibrary library;
                        return &library;
                    }).share();
}

juce::StringArray WaveformLibrary::wavetableNames() const
{
    juce::StringArray names;
    for (const auto& t : wavetables)
        names.add(t->name());
    return names;
}

} // namespace lattice::dsp
