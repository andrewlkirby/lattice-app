#include "engine/TestToneSource.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

using lattice::TestToneSource;

namespace
{
constexpr double sampleRate = 48000.0;

std::vector<float> renderSeconds(TestToneSource& tone, double seconds, int blockSize)
{
    std::vector<float> out(static_cast<std::size_t>(seconds * sampleRate));
    for (std::size_t pos = 0; pos < out.size(); pos += static_cast<std::size_t>(blockSize))
    {
        const auto n = std::min<std::size_t>(static_cast<std::size_t>(blockSize), out.size() - pos);
        tone.render(out.data() + pos, static_cast<int>(n));
    }
    return out;
}

/** Frequency from interpolated upward zero crossings, ignoring the first skipSamples. */
double estimateFrequency(const std::vector<float>& x, std::size_t skipSamples)
{
    double first = -1.0, last = -1.0;
    int crossings = 0;
    for (std::size_t i = std::max<std::size_t>(skipSamples, 1); i < x.size(); ++i)
    {
        if (x[i - 1] < 0.0f && x[i] >= 0.0f)
        {
            const double t = static_cast<double>(i - 1) + x[i - 1] / (x[i - 1] - x[i]);
            if (first < 0.0)
                first = t;
            last = t;
            ++crossings;
        }
    }
    REQUIRE(crossings > 2);
    return (crossings - 1) * sampleRate / (last - first);
}

float maxStep(const std::vector<float>& x)
{
    float m = 0.0f;
    for (std::size_t i = 1; i < x.size(); ++i)
        m = std::max(m, std::abs(x[i] - x[i - 1]));
    return m;
}
} // namespace

TEST_CASE("Test tone plays the requested frequency", "[engine][tone]")
{
    const auto hz = GENERATE(440.0f, 110.0f, 1234.5f);
    const auto block = GENERATE(64, 37, 512);

    TestToneSource tone;
    tone.setFrequency(hz);
    tone.setGain(0.5f);
    tone.setEnabled(true);
    tone.prepare(sampleRate);

    const auto out = renderSeconds(tone, 1.0, block);
    CHECK(estimateFrequency(out, 0) == Catch::Approx(hz).margin(0.1));
}

TEST_CASE("Test tone peak level matches the requested gain", "[engine][tone]")
{
    TestToneSource tone;
    tone.setFrequency(440.0f);
    tone.setGain(0.5f);
    tone.setEnabled(true);
    tone.prepare(sampleRate);

    const auto out = renderSeconds(tone, 1.0, 256);
    const auto settled = std::vector<float>(out.begin() + static_cast<long>(0.1 * sampleRate), out.end());
    const auto peak = *std::max_element(settled.begin(), settled.end(),
                                        [](float a, float b) { return std::abs(a) < std::abs(b); });
    CHECK(std::abs(peak) == Catch::Approx(0.5).margin(0.001));
}

TEST_CASE("Test tone is silent and continuous while disabled", "[engine][tone]")
{
    TestToneSource tone;
    tone.prepare(sampleRate);
    const auto out = renderSeconds(tone, 0.1, 128);
    CHECK(std::all_of(out.begin(), out.end(), [](float s) { return s == 0.0f; }));
}

TEST_CASE("Turning the tone on and off and changing level never clicks", "[engine][tone]")
{
    constexpr float gain = 0.5f;
    constexpr float hz = 440.0f;
    // The steepest step a clean sine of this level can take, plus the gain ramp's own step.
    const float sineStep = gain * static_cast<float>(2.0 * 3.141592653589793 * hz / sampleRate);
    const float rampStep = gain / static_cast<float>(TestToneSource::gainRampSeconds * sampleRate);
    const float limit = 1.05f * (sineStep + rampStep);

    TestToneSource tone;
    tone.setFrequency(hz);
    tone.setGain(gain);
    tone.prepare(sampleRate);

    std::vector<float> out;
    const int block = 64;
    std::vector<float> buffer(block);
    for (int i = 0; i < 1500; ++i) // ~2 s of blocks, toggling at irregular block boundaries
    {
        if (i % 97 == 13)
            tone.setEnabled(!tone.isEnabled());
        if (i % 211 == 50)
            tone.setGain(tone.getGain() > 0.3f ? 0.1f : gain);
        tone.render(buffer.data(), block);
        out.insert(out.end(), buffer.begin(), buffer.end());
    }

    CHECK(maxStep(out) <= limit);
}
