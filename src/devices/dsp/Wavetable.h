#pragma once

#include "core/RealtimeCheck.h"

#include <juce_core/juce_core.h>

#include <array>
#include <complex>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace lattice::dsp
{

/** One single-cycle waveform as a set of band-limited tables, one per half octave of pitch
    (docs/04-m3-devices.md, "Oscillators").

    Pitch is given as a phase increment (cycles per sample as a 32-bit fixed-point fraction), so
    the tables don't depend on the sample rate. The table for a range of increments holds only the
    harmonics that stay below Nyquist for every increment in that range; playback reads it with
    cubic (Catmull-Rom) interpolation and crossfades into the next, sparser table across the top
    half of each range, so moving pitch never switches harmonics on or off abruptly.

    Tables are oversampled 8-16 times relative to their highest harmonic, which keeps the
    interpolation's images (the only source of aliasing) more than 80 dB below the fundamental.
    Immutable after construction; shared between voices and devices. */
class MipmappedWave
{
public:
    /** Harmonic h (1-based) as cos/sin amplitudes: the cycle is sum(re cos(2 pi h x) + im sin(...)). */
    using Spectrum = std::function<std::complex<double>(int harmonic)>;

    static constexpr int numLevels = 25;  // half octaves: increments from 2^-13 to 2^-1 cycles/sample
    static constexpr int guard = 3;       // samples around each table for the interpolator
    static constexpr int minTableBits = 9; // the smallest table: 512 samples

    /** maxHarmonics caps the richest table (2048 keeps the lowest notes full-range at 48 kHz).
        oversampling (8 or 16) is the table length over twice its highest harmonic. Normalises so
        the largest absolute sample of the richest table is `peak`. */
    MipmappedWave(const Spectrum& spectrum, int maxHarmonics, int oversampling = 16, double peak = 1.0);

    /** Where an increment sits: the table to read and how far to blend into the next. */
    struct Position
    {
        int level = 0;
        float blend = 0.0f; // 0 = only `level`; towards 1, more of level + 1
        bool silent = false; // the fundamental itself is above Nyquist
    };

    static Position positionFor(std::uint32_t increment) noexcept LATTICE_NONBLOCKING;

    /** One sample at a phase (0..2^32 = one cycle), at a position from positionFor(). */
    float sample(std::uint32_t phase, const Position& at) const noexcept LATTICE_NONBLOCKING
    {
        if (at.silent)
            return 0.0f;
        const float a = read(levels[static_cast<std::size_t>(at.level)], phase);
        if (at.blend <= 0.0f || at.level + 1 >= numLevels)
            return a;
        const float b = read(levels[static_cast<std::size_t>(at.level + 1)], phase);
        return a + at.blend * (b - a);
    }

    /** The harmonic count of a level (for tests). */
    int harmonicsAt(int level) const noexcept { return levels[static_cast<std::size_t>(level)].harmonics; }
    std::size_t memoryBytes() const noexcept;

private:
    struct Level
    {
        std::vector<float> table; // guard + size + guard samples
        int bits = minTableBits;
        int harmonics = 0;
    };

    static float read(const Level& level, std::uint32_t phase) noexcept LATTICE_NONBLOCKING
    {
        const int shift = 32 - level.bits;
        const auto index = static_cast<int>(phase >> shift);
        const float frac = static_cast<float>(static_cast<std::uint32_t>(phase << level.bits)) * 2.3283064365386963e-10f;
        const float* p = level.table.data() + guard + index - 1;
        // Catmull-Rom
        const float xm1 = p[0];
        const float x0 = p[1];
        const float x1 = p[2];
        const float x2 = p[3];
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * frac + c2) * frac + c1) * frac + x0;
    }

    std::array<Level, numLevels> levels;
};

/** A wavetable: frames of single-cycle waves to morph between (position 0..1 across the frames). */
class Wavetable
{
public:
    Wavetable(juce::String name, std::vector<std::unique_ptr<MipmappedWave>> frames);

    const juce::String& name() const noexcept { return tableName; }
    int numFrames() const noexcept { return static_cast<int>(frames.size()); }
    const MipmappedWave& frame(int index) const noexcept { return *frames[static_cast<std::size_t>(index)]; }

    /** A sample with linear morphing between the two frames around `position` (0..1). */
    float sample(std::uint32_t phase, const MipmappedWave::Position& at, float position) const noexcept
        LATTICE_NONBLOCKING
    {
        const float scaled = juce::jlimit(0.0f, 1.0f, position) * static_cast<float>(frames.size() - 1);
        const int i = juce::jmin(static_cast<int>(scaled), static_cast<int>(frames.size()) - 1);
        const float a = frames[static_cast<std::size_t>(i)]->sample(phase, at);
        if (i + 1 >= static_cast<int>(frames.size()))
            return a;
        const float t = scaled - static_cast<float>(i);
        if (t <= 0.0f)
            return a;
        return a + t * (frames[static_cast<std::size_t>(i + 1)]->sample(phase, at) - a);
    }

private:
    juce::String tableName;
    std::vector<std::unique_ptr<MipmappedWave>> frames;
};

/** The built-in waveforms, built once on first use (thread-safe) and shared by every device. */
class WaveformLibrary
{
public:
    enum class Shape : std::uint8_t
    {
        Sine,
        Triangle,
        Saw
    };

    static const WaveformLibrary& get();
    /** Starts building on a background thread, so the first Synth doesn't wait (call at startup). */
    static void preloadInBackground();

    const MipmappedWave& shape(Shape s) const noexcept { return *shapes[static_cast<std::size_t>(s)]; }
    int numWavetables() const noexcept { return static_cast<int>(wavetables.size()); }
    const Wavetable& wavetable(int index) const noexcept { return *wavetables[static_cast<std::size_t>(index)]; }
    juce::StringArray wavetableNames() const;

private:
    WaveformLibrary();

    std::array<std::unique_ptr<MipmappedWave>, 3> shapes;
    std::vector<std::unique_ptr<Wavetable>> wavetables;
};

/** Phase increment for a frequency (cycles per sample in 32-bit fixed point). */
inline std::uint32_t incrementFor(double hz, double sampleRate) noexcept
{
    const double cycles = hz / sampleRate;
    if (cycles <= 0.0)
        return 0;
    if (cycles >= 0.5)
        return 0x80000000u;
    return static_cast<std::uint32_t>(cycles * 4294967296.0);
}

} // namespace lattice::dsp
