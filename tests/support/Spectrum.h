#pragma once

#include <juce_dsp/juce_dsp.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

namespace lattice::test
{

/** Strongest frequency within ±10% of expectedHz in the first 2^16 samples of signal: Hann window,
    FFT, parabolic interpolation of the log-magnitude peak. Also returns that peak relative to the
    whole spectrum's peak (1 = the fundamental dominates). */
inline std::pair<double, double> measureFundamental(const std::vector<float>& signal, double expectedHz,
                                                    double sampleRate = 48000.0)
{
    constexpr int order = 16;
    constexpr int size = 1 << order;
    REQUIRE(signal.size() >= static_cast<std::size_t>(size));

    std::vector<float> data(2 * size, 0.0f);
    for (int i = 0; i < size; ++i)
    {
        const double w = 0.5 - 0.5 * std::cos(2.0 * juce::MathConstants<double>::pi * i / (size - 1));
        data[static_cast<std::size_t>(i)] = static_cast<float>(signal[static_cast<std::size_t>(i)] * w);
    }
    juce::dsp::FFT fft(order);
    fft.performFrequencyOnlyForwardTransform(data.data());

    const double binHz = sampleRate / size;
    const int lo = std::max(1, static_cast<int>(expectedHz * 0.9 / binHz));
    const int hi = std::min(size / 2 - 2, static_cast<int>(expectedHz * 1.1 / binHz) + 1);
    int peak = lo;
    for (int i = lo; i <= hi; ++i)
        if (data[static_cast<std::size_t>(i)] > data[static_cast<std::size_t>(peak)])
            peak = i;
    const float globalMax = *std::max_element(data.begin() + 1, data.begin() + size / 2);

    const double a = std::log(std::max(1e-12f, data[static_cast<std::size_t>(peak - 1)]));
    const double b = std::log(std::max(1e-12f, data[static_cast<std::size_t>(peak)]));
    const double c = std::log(std::max(1e-12f, data[static_cast<std::size_t>(peak + 1)]));
    const double offset = 0.5 * (a - c) / (a - 2.0 * b + c);
    return {(peak + offset) * binHz, data[static_cast<std::size_t>(peak)] / globalMax};
}

/** Amplitude spectrum of the first 2^order samples with a 4-term Blackman-Harris window (sidelobes
    at -92 dB), scaled so that a sine of amplitude A shows a peak of A. Bin i is i * sr / 2^order. */
inline std::vector<double> amplitudeSpectrum(const std::vector<float>& signal, int order, bool deep = false)
{
    const int size = 1 << order;
    REQUIRE(signal.size() >= static_cast<std::size_t>(size));
    std::vector<float> data(2 * static_cast<std::size_t>(size), 0.0f);
    double windowSum = 0.0;
    // deep: Albrecht's 7-term Blackman-Harris, sidelobes near -180 dB (main lobe about +-7 bins).
    static constexpr double seven[7] = {0.27105140069342, 0.43329793923448, 0.21812299954311, 0.06592544638803,
                                        0.01081174209837, 0.00077658482522, 0.00001388721735};
    for (int i = 0; i < size; ++i)
    {
        const double x = 2.0 * juce::MathConstants<double>::pi * i / size;
        double w = 0.35875 - 0.48829 * std::cos(x) + 0.14128 * std::cos(2 * x) - 0.01168 * std::cos(3 * x);
        if (deep)
        {
            w = 0.0;
            for (int k = 0; k < 7; ++k)
                w += (k % 2 == 0 ? 1.0 : -1.0) * seven[k] * std::cos(k * x);
        }
        windowSum += w;
        data[static_cast<std::size_t>(i)] = static_cast<float>(signal[static_cast<std::size_t>(i)] * w);
    }
    juce::dsp::FFT fft(order);
    fft.performFrequencyOnlyForwardTransform(data.data());
    std::vector<double> amplitudes(static_cast<std::size_t>(size / 2));
    for (int i = 0; i < size / 2; ++i)
        amplitudes[static_cast<std::size_t>(i)] = 2.0 * data[static_cast<std::size_t>(i)] / windowSum;
    return amplitudes;
}

/** Aliasing of a periodic signal with fundamental f0: the strongest component that is not near a
    harmonic (or DC), in dB relative to the fundamental. */
inline double aliasingDb(const std::vector<float>& signal, double f0, double sampleRate = 48000.0, int order = 16,
                         bool deep = false)
{
    const auto spectrum = amplitudeSpectrum(signal, order, deep);
    const double binHz = sampleRate / (1 << order);
    const int guardBins = deep ? 10 : 6; // the window's main lobe and its skirt
    const auto nearHarmonic = [&](int bin) {
        const double hz = bin * binHz;
        const double k = std::round(hz / f0);
        return std::abs(hz - k * f0) <= guardBins * binHz;
    };
    double fundamental = 0.0;
    const int f0Bin = static_cast<int>(std::round(f0 / binHz));
    for (int b = std::max(1, f0Bin - guardBins); b <= f0Bin + guardBins; ++b)
        fundamental = std::max(fundamental, spectrum[static_cast<std::size_t>(b)]);
    double worst = 0.0;
    for (int b = guardBins + 1; b < static_cast<int>(spectrum.size()); ++b)
        if (!nearHarmonic(b))
            worst = std::max(worst, spectrum[static_cast<std::size_t>(b)]);
    return 20.0 * std::log10(std::max(worst, 1e-15) / fundamental);
}

} // namespace lattice::test
