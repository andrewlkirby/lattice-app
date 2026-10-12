#pragma once

#include "core/RealtimeCheck.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace lattice::dsp
{

/** An ADSR envelope with shaped segments (docs/04-m3-devices.md, "ADSR").

    Each segment runs from its start value to its target in exactly its time (to the sample): a
    one-pole recursion aimed past the target by an "overshoot" that sets the curve, stopped where it
    crosses the target. Curve 0 is close to linear; higher values bend the decay and release
    towards the exponential shape of analog envelopes, and the attack towards a fast-rising curve.
    One multiply-add per sample. */
class Adsr
{
public:
    enum class Stage : std::uint8_t
    {
        Idle,
        Attack,
        Decay,
        Sustain,
        Release
    };

    struct Settings
    {
        float attackSeconds = 0.005f;
        float decaySeconds = 0.3f;
        float sustain = 0.7f; // 0..1
        float releaseSeconds = 0.3f;
        float attackCurve = 0.3f; // 0 (linear) .. 1 (strongly bent)
        float decayCurve = 0.7f;
    };

    void setSampleRate(double sr) noexcept { sampleRate = sr; }

    /** Takes effect for segments that start from now on; the sustain level follows at once. */
    void setSettings(const Settings& s) noexcept { settings = s; }

    void noteOn() noexcept LATTICE_NONBLOCKING { startSegment(Stage::Attack); }
    void noteOff() noexcept LATTICE_NONBLOCKING
    {
        if (stage != Stage::Idle && stage != Stage::Release)
            startSegment(Stage::Release);
    }
    /** Fast fade (voice stealing): to 0 in `seconds`, linear. */
    void kill(float seconds) noexcept LATTICE_NONBLOCKING
    {
        stage = Stage::Release;
        remaining = std::max(1, static_cast<int>(seconds * sampleRate));
        coefficient = 1.0f;
        offset = -value / static_cast<float>(remaining);
        target = 0.0f;
    }
    void reset() noexcept
    {
        stage = Stage::Idle;
        value = 0.0f;
        remaining = 0;
    }

    Stage currentStage() const noexcept { return stage; }
    bool isActive() const noexcept { return stage != Stage::Idle; }
    float currentValue() const noexcept { return value; }

    float next() noexcept LATTICE_NONBLOCKING
    {
        switch (stage)
        {
        case Stage::Idle:
            return 0.0f;
        case Stage::Sustain:
            value += 0.002f * (settings.sustain - value); // follows sustain changes without zipper
            return value;
        case Stage::Attack:
        case Stage::Decay:
        case Stage::Release:
            break;
        }
        value = value * coefficient + offset;
        if (--remaining <= 0)
        {
            value = target;
            advance();
        }
        return value;
    }

    /** The next n values into out: the same values next() gives, with one tight loop per segment. */
    void renderBlock(float* out, int n) noexcept LATTICE_NONBLOCKING
    {
        int i = 0;
        while (i < n)
        {
            switch (stage)
            {
            case Stage::Idle:
                for (; i < n; ++i)
                    out[i] = 0.0f;
                return;
            case Stage::Sustain:
                for (; i < n; ++i)
                {
                    value += 0.002f * (settings.sustain - value);
                    out[i] = value;
                }
                return;
            case Stage::Attack:
            case Stage::Decay:
            case Stage::Release:
                break;
            }
            const int k = std::min(n - i, remaining);
            float v = value;
            const float c = coefficient;
            const float o = offset;
            for (int j = 0; j < k - 1; ++j)
            {
                v = v * c + o;
                out[i + j] = v;
            }
            // The segment's last sample of this run goes through next(), which ends the segment.
            value = v;
            remaining -= k - 1;
            out[i + k - 1] = next();
            i += k;
        }
    }

    /** Skips n samples (a modulation envelope read at control rate) and returns the value reached. */
    float advance(int n) noexcept LATTICE_NONBLOCKING
    {
        while (n > 0)
        {
            switch (stage)
            {
            case Stage::Idle:
                return 0.0f;
            case Stage::Sustain:
                value += (1.0f - std::pow(0.998f, static_cast<float>(n))) * (settings.sustain - value);
                return value;
            case Stage::Attack:
            case Stage::Decay:
            case Stage::Release:
                break;
            }
            const int k = std::min(n, remaining - 1);
            if (k > 0)
            {
                if (coefficient == 1.0f)
                {
                    value += offset * static_cast<float>(k);
                }
                else
                {
                    const float aim = offset / (1.0f - coefficient);
                    value = aim + (value - aim) * static_cast<float>(std::pow(static_cast<double>(coefficient), k));
                }
                remaining -= k;
                n -= k;
            }
            if (n > 0)
            {
                next(); // the segment's last sample, which moves to the next stage
                --n;
            }
        }
        return value;
    }

private:
    void advance() noexcept
    {
        switch (stage)
        {
        case Stage::Attack:
            startSegment(Stage::Decay);
            break;
        case Stage::Decay:
            stage = Stage::Sustain;
            break;
        case Stage::Release:
            stage = Stage::Idle;
            value = 0.0f;
            break;
        case Stage::Idle:
        case Stage::Sustain:
            break;
        }
    }

    void startSegment(Stage s) noexcept
    {
        stage = s;
        float seconds = 0.0f;
        float curve = settings.decayCurve;
        switch (s)
        {
        case Stage::Attack:
            seconds = settings.attackSeconds;
            target = 1.0f;
            curve = settings.attackCurve;
            break;
        case Stage::Decay:
            seconds = settings.decaySeconds;
            target = settings.sustain;
            break;
        case Stage::Release:
            seconds = settings.releaseSeconds;
            target = 0.0f;
            break;
        case Stage::Idle:
        case Stage::Sustain:
            return;
        }
        remaining = std::max(1, static_cast<int>(std::lround(seconds * sampleRate)));
        if (s == Stage::Decay && remaining <= 1)
        {
            value = target;
            stage = Stage::Sustain;
            return;
        }

        // Aim past the target by `overshoot` (relative to the segment's span): small overshoot is
        // a strongly bent curve, large is nearly linear.
        const float span = target - value;
        const float overshoot = 0.0001f + std::pow(1.0f - std::clamp(curve, 0.0f, 1.0f), 3.0f) * 50.0f;
        const float aim = target + span * overshoot;
        // value_n = aim + (start - aim) c^n; reach the target at n = remaining.
        const float startDistance = value - aim;
        const float endDistance = target - aim;
        if (std::abs(startDistance) < 1.0e-9f || std::abs(span) < 1.0e-9f)
        {
            coefficient = 1.0f;
            offset = 0.0f;
            return;
        }
        coefficient = static_cast<float>(std::pow(static_cast<double>(endDistance / startDistance),
                                                  1.0 / static_cast<double>(remaining)));
        offset = aim * (1.0f - coefficient);
    }

    Settings settings;
    double sampleRate = 48000.0;
    Stage stage = Stage::Idle;
    float value = 0.0f;
    float target = 0.0f;
    float coefficient = 1.0f;
    float offset = 0.0f;
    int remaining = 0;
};

} // namespace lattice::dsp
