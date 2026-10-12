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

} // namespace lattice::test
