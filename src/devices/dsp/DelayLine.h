#pragma once

#include "core/RealtimeCheck.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace lattice::dsp
{

/** A fractional delay line (docs/04-m3-devices.md, "Delay lines"). The buffer is a power of two,
    sized in prepare() (message thread) for the longest delay; reading interpolates with a 4-point,
    3rd-order Lagrange interpolator (flat, low phase error) or linearly. The first samples are
    mirrored past the end, so the four points of any read sit next to each other in memory
    (window()), for SIMD readers. */
class DelayLine
{
public:
    /** Not on the audio thread. */
    void prepare(int maxDelaySamples)
    {
        int size = 4;
        while (size < maxDelaySamples + 4)
            size <<= 1;
        buffer.assign(static_cast<std::size_t>(size + guard), 0.0f);
        mask = size - 1;
        writePos = 0;
    }

    void reset() noexcept { std::fill(buffer.begin(), buffer.end(), 0.0f); }
    int capacity() const noexcept { return mask - 3; }

    void push(float x) noexcept LATTICE_NONBLOCKING
    {
        writePos = (writePos + 1) & mask;
        buffer[static_cast<std::size_t>(writePos)] = x;
        if (writePos < guard)
            buffer[static_cast<std::size_t>(mask + 1 + writePos)] = x;
    }

    /** n samples, oldest first: as n calls of push(). */
    void pushBlock(const float* x, int n) noexcept LATTICE_NONBLOCKING
    {
        for (int done = 0; done < n;)
        {
            const int at = (writePos + 1) & mask;
            const int run = std::min(n - done, mask + 1 - at);
            std::copy(x + done, x + done + run, buffer.begin() + at);
            for (int i = at; i < std::min(at + run, guard); ++i)
                buffer[static_cast<std::size_t>(mask + 1 + i)] = buffer[static_cast<std::size_t>(i)];
            writePos = (at + run - 1) & mask;
            done += run;
        }
    }

    /** The n samples from delay `newest + n - 1` down to delay `newest`, oldest first, in at most
        two runs of memory: f(samples, count, index of the first among the n). newest + n - 1 must
        be at most capacity(). */
    template <typename F>
    void forEachRun(int newest, int n, F&& f) const noexcept LATTICE_NONBLOCKING
    {
        const int start = (writePos - newest - (n - 1)) & mask;
        const int first = std::min(n, mask + 1 - start);
        f(buffer.data() + start, first, 0);
        if (first < n)
            f(buffer.data(), n - first, first);
    }

    /** The four points around delay `whole` in memory order: delays whole + 2, whole + 1, whole and
        whole - 1 (what read() interpolates between). */
    const float* window(int whole) const noexcept LATTICE_NONBLOCKING
    {
        return buffer.data() + ((writePos - whole - 2) & mask);
    }

    /** The sample pushed `delay` samples ago (integer). delay 0 is the latest. */
    float tap(int delay) const noexcept LATTICE_NONBLOCKING
    {
        return buffer[static_cast<std::size_t>((writePos - delay) & mask)];
    }

    /** Fractional delay, Lagrange-interpolated. delay >= 1 keeps the interpolator causal. */
    float read(float delay) const noexcept LATTICE_NONBLOCKING
    {
        delay = std::clamp(delay, 1.0f, static_cast<float>(capacity()));
        const int whole = static_cast<int>(delay);
        const float d = delay - static_cast<float>(whole); // 0..1
        // Points at delays whole-1 .. whole+2; the fractional position is 1 + d among them.
        const float ym1 = tap(whole - 1);
        const float y0 = tap(whole);
        const float y1 = tap(whole + 1);
        const float y2 = tap(whole + 2);
        const float x = d;
        // Third-order Lagrange through x = -1, 0, 1, 2.
        const float c0 = y0;
        const float c1 = y1 - ym1 / 3.0f - 0.5f * y0 - y2 / 6.0f;
        const float c2 = 0.5f * (ym1 + y1) - y0;
        const float c3 = (y2 - ym1) / 6.0f + 0.5f * (y0 - y1);
        return ((c3 * x + c2) * x + c1) * x + c0;
    }

    float readLinear(float delay) const noexcept LATTICE_NONBLOCKING
    {
        delay = std::clamp(delay, 0.0f, static_cast<float>(capacity()));
        const int whole = static_cast<int>(delay);
        const float d = delay - static_cast<float>(whole);
        const float a = tap(whole);
        return a + d * (tap(whole + 1) - a);
    }

private:
    static constexpr int guard = 3;
    std::vector<float> buffer{0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    int mask = 3;
    int writePos = 0;
};

/** Smooths a control value towards a target, linearly over a fixed time. */
class LinearRamp
{
public:
    void prepare(double sampleRate, double seconds) noexcept
    {
        steps = std::max(1, static_cast<int>(sampleRate * seconds));
    }
    void setImmediately(float v) noexcept
    {
        current = target = v;
        remaining = 0;
    }
    void setTarget(float v) noexcept LATTICE_NONBLOCKING
    {
        if (v == target)
            return;
        target = v;
        remaining = steps;
        step = (target - current) / static_cast<float>(steps);
    }
    float next() noexcept LATTICE_NONBLOCKING
    {
        if (remaining > 0)
        {
            current += step;
            if (--remaining == 0)
                current = target;
        }
        return current;
    }
    /** Advances n samples at once (for block-rate users). */
    float skip(int n) noexcept LATTICE_NONBLOCKING
    {
        if (remaining > 0)
        {
            const int k = std::min(n, remaining);
            current += step * static_cast<float>(k);
            remaining -= k;
            if (remaining == 0)
                current = target;
        }
        return current;
    }
    bool isRamping() const noexcept { return remaining > 0; }
    float value() const noexcept { return current; }
    float targetValue() const noexcept { return target; }

private:
    float current = 0.0f;
    float target = 0.0f;
    float step = 0.0f;
    int steps = 1;
    int remaining = 0;
};

/** Peak or RMS level detector with separate attack and release (one-pole), for compressors,
    envelope followers and meters. */
class LevelDetector
{
public:
    void setTimes(double sampleRate, double attackSeconds, double releaseSeconds) noexcept
    {
        attackCoef = coefFor(sampleRate, attackSeconds);
        releaseCoef = coefFor(sampleRate, releaseSeconds);
    }
    void reset() noexcept { state = 0.0f; }

    /** Peak: follows |x|. */
    float peak(float x) noexcept LATTICE_NONBLOCKING
    {
        const float in = std::abs(x);
        const float c = in > state ? attackCoef : releaseCoef;
        state = in + c * (state - in);
        return state;
    }

    /** RMS: follows x^2; returns the root. */
    float rms(float x) noexcept LATTICE_NONBLOCKING
    {
        const float in = x * x;
        const float c = in > state ? attackCoef : releaseCoef;
        state = in + c * (state - in);
        return std::sqrt(state);
    }

    float value() const noexcept { return state; }

private:
    static float coefFor(double sampleRate, double seconds) noexcept
    {
        return seconds <= 0.0 ? 0.0f : static_cast<float>(std::exp(-1.0 / (seconds * sampleRate)));
    }

    float state = 0.0f;
    float attackCoef = 0.0f;
    float releaseCoef = 0.0f;
};

} // namespace lattice::dsp
